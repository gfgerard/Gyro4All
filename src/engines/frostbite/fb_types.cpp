#include "fb_types.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_set>

namespace gyro4all::fb {

namespace {

struct Image {
    uintptr_t begin = 0, end = 0;
};

Image exe_image() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

bool readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Calls f(address, data, size) for every committed, readable piece of the exe image, copied in chunks (the protector
// may change page protections at any time: a piece that can't be copied is skipped). One scan at a time: the attach
// thread (camera models) and the game's DirectInput8Create (fb_xinput.cpp) may scan together, and they share the buffer.
template <class F>
void scan_image(const Image& image, F f) {
    constexpr size_t kChunk = 4 << 20;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    static std::vector<uint8_t> buffer(kChunk);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t at = image.begin; at < image.end && VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi);) {
        const uintptr_t region_end = std::min(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, image.end);
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect)) {
            for (uintptr_t p = at; p < region_end; p += kChunk) {
                const size_t n = std::min<size_t>(kChunk, region_end - p);
                if (mem::guarded_copy(buffer.data(), p, n)) f(p, buffer.data(), n);
            }
        }
        at = region_end;
    }
}

// Addresses of the 8-byte aligned qwords of the image whose value is in values.
std::vector<uintptr_t> find_qwords(const Image& image, const std::unordered_set<uintptr_t>& values) {
    std::vector<uintptr_t> out;
    if (values.empty()) return out;
    const uintptr_t lo = *std::min_element(values.begin(), values.end());
    const uintptr_t hi = *std::max_element(values.begin(), values.end());
    scan_image(image, [&](uintptr_t address, const uint8_t* data, size_t n) {
        const auto* q = reinterpret_cast<const uint64_t*>(data);
        for (size_t i = 0; i < n / 8; ++i)
            if (q[i] >= lo && q[i] <= hi && values.count(q[i])) out.push_back(address + i * 8);
    });
    return out;
}

// Addresses of "\0<text>\0" in the image (of <text>'s first character).
std::vector<uintptr_t> find_strings(const Image& image, const char* text) {
    std::vector<uintptr_t> out;
    const size_t len = std::strlen(text) + 1;
    scan_image(image, [&](uintptr_t address, const uint8_t* data, size_t n) {
        for (size_t i = 1; i + len <= n; ++i) {
            const void* hit = std::memchr(data + i, text[0], n - len + 1 - i);
            if (!hit) break;
            i = static_cast<const uint8_t*>(hit) - data;
            if (data[i - 1] == 0 && std::memcmp(data + i, text, len) == 0) out.push_back(address + i);
        }
    });
    return out;
}

bool in_image(const Image& image, uintptr_t p) {
    return p >= image.begin && p < image.end;
}

// A type or field name: printable identifier-ish text, 1..127 characters (0 with allow_empty: the 2013 layout's array
// types).
bool read_name(uintptr_t p, std::string& out, bool allow_empty = false) {
    char buffer[128];
    if (!p) return false;
    // Read what can be read up to 128 bytes (the name may end near the end of a page).
    size_t n = 0;
    for (; n < sizeof buffer; n += 16)
        if (!mem::guarded_copy(buffer + n, p + n, 16)) break;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (c == 0) {
            if (i == 0 && !allow_empty) return false;
            out.assign(buffer, i);
            return true;
        }
        if (!(std::isalnum(c) || std::strchr("_:<>,. -$*&[]", c))) return false;
    }
    return false;
}

// The info data and name of the TypeInfo at t, if its info data (at info_offset) starts with a name (possibly empty:
// the 2013 layout's array types).
bool read_type(uintptr_t t, uint32_t info_offset, uintptr_t& info, std::string& name) {
    info = t ? mem::rdp(t + info_offset) : 0;
    // Names are packed strings: their pointers are byte-aligned.
    return info && read_name(mem::rdp(info, 1), name, true);
}

bool is_type(uintptr_t t, uint32_t info_offset) {
    uintptr_t info = 0;
    std::string name;
    return read_type(t, info_offset, info, name);
}

// How many TypeInfos follow t through the link at offset (up to limit), and whether the walk ended on a null link.
size_t walk(uintptr_t t, uint32_t info_offset, uint32_t offset, size_t limit, bool& ended) {
    std::unordered_set<uintptr_t> seen{t};
    ended = false;
    size_t n = 0;
    for (; n < limit; ++n) {
        uintptr_t next = 0;
        if (!mem::rd(t + offset, next)) return n;
        if (!next) return ended = true, n;
        if (!seen.insert(next).second || !is_type(next, info_offset)) return n;
        t = next;
    }
    return n;
}

// The 2021+ info data layout (Frosty's Bf2042.cs): name, hash, flags, size, guid, namespace, array type, alignment,
// field count, signature, then 7 pointers (parent class or enum values, class fields, ..., struct fields).
struct RawInfo {
    uint64_t name;
    uint32_t hash;
    uint16_t flags, size;
    uint8_t guid[16];
    uint64_t name_space, array_type;
    uint16_t alignment, field_count;
    uint32_t signature;
    uint64_t pointers[7];
};
static_assert(sizeof(RawInfo) == 0x70);

struct RawField {
    uint64_t name;
    uint32_t hash;
    uint16_t flags, offset;
    uint64_t type;
};
static_assert(sizeof(RawField) == 0x18);

bool read_layout_2021(TypeDesc& t) {
    RawInfo raw{};
    if (!mem::rd(t.info, raw)) return false;
    t.kind = (raw.flags >> 1 >> 4) & 0x1F;
    t.size = raw.size;
    t.alignment = raw.alignment;
    t.array_type = mem::plausible(raw.array_type) ? raw.array_type : 0;
    // The namespace field points to a record whose first field is the name (Frosty reads it the same way).
    if (mem::plausible(raw.name_space)) read_name(mem::rdp(raw.name_space, 1), t.name_space);
    uint64_t fields = 0;
    if (t.kind == Class) t.parent = raw.pointers[0], fields = raw.pointers[1];
    else if (t.kind == Struct) fields = raw.pointers[6];
    else if (t.kind == Enum) fields = raw.pointers[0];
    if (t.parent == t.type || !mem::plausible(t.parent)) t.parent = 0;
    if (!fields || !mem::plausible(fields) || raw.field_count > 4096) return true;
    std::vector<RawField> raws(raw.field_count);
    if (!mem::guarded_copy(raws.data(), fields, raws.size() * sizeof(RawField))) return true;
    for (const RawField& f : raws) {
        FieldDesc d;
        if (!read_name(f.name, d.name)) break;
        d.offset = f.offset;
        d.flags = f.flags;
        d.type = f.type;
        t.fields.push_back(std::move(d));
    }
    return true;
}

// The 2013 info data layout (Battlefield 4, from the running game): name, flags, size, module (a record whose first
// field is its name, e.g. "Core"), alignment, field count, then by kind: classes the parent class, a create function
// and the fields; structs a default value and the fields; enums the values; arrays the element type. The TypeInfo
// itself is info data, next TypeInfo, runtime id and flags (classes: then the parent, default instance, class id, first
// derived and next sibling class).
struct RawInfo2013 {
    uint64_t name;
    uint16_t flags, size;
    uint32_t pad;
    uint64_t module;
    uint16_t alignment, field_count;
    uint32_t pad2;
    uint64_t pointers[3];
};
static_assert(sizeof(RawInfo2013) == 0x38);

struct RawField2013 {
    uint64_t name;
    uint16_t flags, offset;
    uint32_t pad;
    uint64_t type;  // enums: the value
};
static_assert(sizeof(RawField2013) == 0x18);

bool read_layout_2013(TypeDesc& t) {
    RawInfo2013 raw{};
    // Struct and enum info data end before pointers[2]: read what's there.
    if (!mem::rd(t.info, raw) && !mem::guarded_copy(&raw, t.info, offsetof(RawInfo2013, pointers) + 16)) return false;
    t.kind = (raw.flags >> 4) & 0x1F;
    t.size = raw.size;
    t.alignment = raw.alignment;
    if (mem::plausible(raw.module)) read_name(mem::rdp(raw.module, 1), t.name_space);
    uint64_t fields = 0;
    if (t.kind == Class) t.parent = raw.pointers[0], fields = raw.pointers[2];
    else if (t.kind == Struct) fields = raw.pointers[1];
    else if (t.kind == Enum) fields = raw.pointers[0];
    else if (t.kind == Array && mem::plausible(raw.pointers[0])) t.element = raw.pointers[0];
    if (t.parent == t.type || !mem::plausible(t.parent)) t.parent = 0;
    if (!fields || !mem::plausible(fields) || raw.field_count > 4096) return true;
    std::vector<RawField2013> raws(raw.field_count);
    if (!mem::guarded_copy(raws.data(), fields, raws.size() * sizeof(RawField2013))) return true;
    for (const RawField2013& f : raws) {
        FieldDesc d;
        if (!read_name(f.name, d.name)) break;
        d.offset = f.offset;
        d.flags = f.flags;
        d.type = f.type;
        t.fields.push_back(std::move(d));
    }
    return true;
}

// Whether info data has the 2013 shape (checked on DataContainer, so that a 2021 one isn't read as 2013: there the
// hash, guid and namespace sit where this layout has flags, padding and module).
bool shaped_2013(uintptr_t info) {
    RawInfo2013 raw{};
    return mem::rd(info, raw) && (raw.flags & 1) && raw.pad == 0 && raw.pad2 == 0 && mem::plausible(raw.module) &&
           raw.alignment && raw.alignment <= 64 && (raw.alignment & (raw.alignment - 1)) == 0;
}

// The 2017 info data layout (Mass Effect: Andromeda, worked out from Gyro4All-types.txt's raw info data): the 2013 one
// with the array type's TypeInfo after the module, so alignment, field count and the pointers are 8 bytes later.
// Classes: parent, fields, then functions; enums: the values; arrays: the element type; structs: a few pointers (two
// functions, a default value) before the fields, found by their shape. Arrays have their own name ("<element>-Array").
// The fields are read as 2013 ones.
struct RawInfo2017 {
    uint64_t name;
    uint16_t flags, size;
    uint32_t pad;
    uint64_t module;
    uint64_t array_type;
    uint16_t alignment, field_count;
    uint32_t pad2;
    uint64_t pointers[4];
};
static_assert(sizeof(RawInfo2017) == 0x48);

// The fields at p, if the first one has a name and an offset inside the type.
bool read_fields_2013(uint64_t p, uint32_t count, uint32_t size, std::vector<FieldDesc>& out) {
    out.clear();
    if (!p || !mem::plausible(p) || !count || count > 4096) return false;
    std::vector<RawField2013> raws(count);
    if (!mem::guarded_copy(raws.data(), p, raws.size() * sizeof(RawField2013))) return false;
    for (const RawField2013& f : raws) {
        FieldDesc d;
        if (!read_name(f.name, d.name)) break;
        d.offset = f.offset;
        d.flags = f.flags;
        d.type = f.type;
        out.push_back(std::move(d));
    }
    return !out.empty() && (!size || out.front().offset < size);
}

bool read_layout_2017(TypeDesc& t) {
    RawInfo2017 raw{};
    // Array and enum info data end before pointers[1]: read what's there.
    if (!mem::rd(t.info, raw) && !mem::guarded_copy(&raw, t.info, offsetof(RawInfo2017, pointers) + 8)) return false;
    t.kind = (raw.flags >> 4) & 0x1F;
    t.size = raw.size;
    t.alignment = raw.alignment;
    t.array_type = mem::plausible(raw.array_type) ? raw.array_type : 0;
    if (mem::plausible(raw.module)) read_name(mem::rdp(raw.module, 1), t.name_space);
    if (t.kind == Class) {
        t.parent = raw.pointers[0];
        read_fields_2013(raw.pointers[1], raw.field_count, t.size, t.fields);
    } else if (t.kind == Struct) {
        for (int i : {3, 2, 1, 0})
            if (read_fields_2013(raw.pointers[i], raw.field_count, t.size, t.fields)) break;
    } else if (t.kind == Enum) {
        read_fields_2013(raw.pointers[0], raw.field_count, 0, t.fields);
    } else if (t.kind == Array && mem::plausible(raw.pointers[0])) {
        t.element = raw.pointers[0];
    }
    if (t.parent == t.type || !mem::plausible(t.parent)) t.parent = 0;
    return true;
}

// Whether info data has the 2017 shape (checked on DataContainer): the 2013 checks with the array type (null or a
// pointer) where the 2013 layout has alignment and field count.
bool shaped_2017(uintptr_t info) {
    RawInfo2017 raw{};
    return mem::rd(info, raw) && (raw.flags & 1) && raw.pad == 0 && raw.pad2 == 0 && mem::plausible(raw.module) &&
           (!raw.array_type || mem::plausible(raw.array_type)) && raw.alignment && raw.alignment <= 64 &&
           (raw.alignment & (raw.alignment - 1)) == 0;
}

bool read_layout(TypeDesc& t, uint32_t year) {
    return year == 2013 ? read_layout_2013(t) : year == 2017 ? read_layout_2017(t) : read_layout_2021(t);
}

// A TypeInfo of the 2013 layout: info data in the image starting with a name in the image (empty for arrays), flags
// with bit 0 set (every type of Battlefield 4 has it), next TypeInfo null or in the image.
bool is_type_2013(const Image& image, uintptr_t t) {
    uintptr_t info = 0, next = 0;
    std::string name;
    uint16_t flags = 0;
    return mem::rd(t + 8, next) && (!next || in_image(image, next)) && read_type(t, 0, info, name) &&
           in_image(image, info) && in_image(image, mem::rdp(info, 1)) && mem::rd(info + 8, flags) && (flags & 1);
}

// The 2013 layout's types that are newer than seed: the list is single-linked from the newest type, so they are the
// TypeInfos whose next is a known one, found in one pass over the image's pointers into the image (each sorted by
// the address it points to), then repeated on each newly found TypeInfo.
void find_newer_2013(const Image& image, std::vector<uintptr_t>& found, std::unordered_set<uintptr_t>& seen) {
    std::vector<std::pair<uintptr_t, uintptr_t>> refs;  // (value, address)
    scan_image(image, [&](uintptr_t address, const uint8_t* data, size_t n) {
        const auto* q = reinterpret_cast<const uint64_t*>(data);
        for (size_t i = 0; i < n / 8; ++i)
            if (in_image(image, q[i])) refs.emplace_back(q[i], address + i * 8);
    });
    std::sort(refs.begin(), refs.end());
    for (size_t i = 0; i < found.size(); ++i) {
        auto it = std::lower_bound(refs.begin(), refs.end(), std::make_pair(found[i], uintptr_t(0)));
        for (; it != refs.end() && it->first == found[i]; ++it) {
            const uintptr_t t = it->second - 8;
            if (!seen.count(t) && is_type_2013(image, t)) seen.insert(t), found.push_back(t);
        }
    }
}

const char* kind_name(uint32_t kind) {
    static const char* const names[] = {
        "inherited", "DbObject", "struct",  "class",  "array",  "fixed array", "string",   "CString",
        "enum",      "FileRef",  "bool",    "int8",   "uint8",  "int16",       "uint16",   "int32",
        "uint32",    "int64",    "uint64",  "float",  "double", "Guid",        "Sha1",     "ResourceRef",
        "function",  "TypeRef",  "BoxedValueRef", "interface", "delegate"};
    return kind < std::size(names) ? names[kind] : "?";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

const TypeDesc* TypeTable::find(const std::string& name) const {
    for (const TypeDesc& t : types)
        if (t.name == name) return &t;
    return nullptr;
}

std::string TypeTable::type_name(uintptr_t type) const {
    if (const TypeDesc* t = find(type)) return t->name;
    uintptr_t info = 0;
    std::string name;
    return read_type(type, info_offset, info, name) ? name : std::format("?{:X}", type);
}

bool find_types(TypeTable& out, std::string& why) {
    const Image image = exe_image();
    out = {};
    // 1. The name "DataContainer", 2. its info data (name first), 3. the TypeInfos that point to that info data.
    const std::vector<uintptr_t> names = find_strings(image, "DataContainer");
    if (names.empty()) return why = "no \"DataContainer\" string in the exe yet", false;
    const std::vector<uintptr_t> infos = find_qwords(image, {names.begin(), names.end()});
    if (infos.empty()) return why = std::format("{} \"DataContainer\" strings, none referenced", names.size()), false;
    const std::vector<uintptr_t> refs = find_qwords(image, {infos.begin(), infos.end()});

    // 4. The TypeInfo is the one whose links lead to other TypeInfos: try the info data at a few offsets, and each
    // pointer around it as a link. A link counts when a walk along it meets 32 TypeInfos, or ends on null after 4.
    uintptr_t seed = 0;
    for (uintptr_t ref : refs) {
        for (uint32_t k = 0; k <= 0x18 && !seed; k += 8) {
            const uintptr_t t = ref - k;
            std::vector<uint32_t> links;
            for (uint32_t o = 0; o <= k + 0x30; o += 8) {
                if (o == k) continue;
                bool ended = false;
                const size_t n = walk(t, k, o, 32, ended);
                if (n >= 32 || (ended && n >= 4)) links.push_back(o);
            }
            if (!links.empty()) {
                seed = t;
                out.info_offset = k;
                out.links = links;
            }
        }
        if (seed) break;
    }
    if (!seed)
        return why = std::format("{} \"DataContainer\" names, {} info data, {} references, no linked TypeInfo",
                                 names.size(), infos.size(), refs.size()),
               false;
    // 5. The info data layout, checked on DataContainer: a class of plausible size whose parent is a type (or itself,
    // which read_layout clears). The 2013 layout first: its TypeInfo has the info data at +0 and one link, at +8.
    const auto fits = [&](uint32_t year, TypeDesc& probe) {
        probe = {};
        probe.type = seed;
        return read_type(seed, out.info_offset, probe.info, probe.name) && read_layout(probe, year) &&
               probe.kind == Class && probe.size >= 8 && probe.size < 0x1000 &&
               (!probe.parent || is_type(probe.parent, out.info_offset));
    };
    TypeDesc probe;
    const bool has_next = std::find(out.links.begin(), out.links.end(), 8u) != out.links.end();
    if (out.info_offset == 0 && has_next && fits(2013, probe) && shaped_2013(probe.info))
        out.layout_year = 2013, out.links = {8};
    else if (out.info_offset == 0 && has_next && fits(2017, probe) && shaped_2017(probe.info))
        out.layout_year = 2017, out.links = {8};
    else if (fits(2021, probe)) out.layout_year = 2021;
    out.layout = out.layout_year != 0;
    std::string links;
    for (uint32_t o : out.links) links += std::format(" +0x{:X}", o);
    log("fb: DataContainer TypeInfo at %s, info data at +0x%X, links at%s, info data layout %s",
        mem::module_offset(seed).c_str(), out.info_offset, links.c_str(),
        out.layout ? std::to_string(out.layout_year).c_str() : "unknown");

    // 6. Every type: everything the links reach from DataContainer, and with the 2013 and 2017 layouts (one list) the
    // newer types too.
    std::vector<uintptr_t> found{seed};
    std::unordered_set<uintptr_t> seen{seed};
    for (size_t i = 0; i < found.size(); ++i)
        for (uint32_t o : out.links) {
            const uintptr_t n = mem::rdp(found[i] + o);
            if (n && is_type(n, out.info_offset) && seen.insert(n).second) found.push_back(n);
        }
    const size_t older = found.size();
    if (out.layout_year == 2013 || out.layout_year == 2017) find_newer_2013(image, found, seen);
    for (uintptr_t t : found) {
        TypeDesc d;
        d.type = t;
        if (!read_type(t, out.info_offset, d.info, d.name)) continue;
        if (out.layout) read_layout(d, out.layout_year);
        out.by_type[t] = out.types.size();
        out.types.push_back(std::move(d));
    }
    // The 2013 layout's arrays have no name: "<element>-Array", and the element gets its array type.
    for (TypeDesc& t : out.types) {
        if (t.kind != Array || !t.element || !t.name.empty()) continue;
        t.name = out.type_name(t.element) + "-Array";
        const auto it = out.by_type.find(t.element);
        if (it != out.by_type.end()) out.types[it->second].array_type = t.type;
    }
    log("fb: %zu types (%zu from DataContainer on, %zu newer) (DataContainer: kind %u, size 0x%X, parent %s, %zu "
        "fields)",
        out.types.size(), older, found.size() - older, probe.kind, probe.size,
        probe.parent ? out.type_name(probe.parent).c_str() : "none", probe.fields.size());
    std::sort(out.types.begin(), out.types.end(), [](const TypeDesc& a, const TypeDesc& b) { return a.name < b.name; });
    out.by_type.clear();
    for (size_t i = 0; i < out.types.size(); ++i) out.by_type[out.types[i].type] = i;
    return true;
}

std::vector<uintptr_t> find_gettype(uintptr_t type) {
    std::vector<uintptr_t> out;
    scan_image(exe_image(), [&](uintptr_t address, const uint8_t* data, size_t n) {
        // 48 8D 05 rel32 C3 (a pattern across two chunks is missed: chunks are 4 MB, getType is 8 bytes).
        for (size_t i = 0; i + 8 <= n; ++i) {
            if (data[i] != 0x48 || data[i + 1] != 0x8D || data[i + 2] != 0x05 || data[i + 7] != 0xC3) continue;
            int32_t rel = 0;
            std::memcpy(&rel, data + i + 3, 4);
            if (address + i + 7 + rel == type) out.push_back(address + i);
        }
    });
    return out;
}

std::vector<uintptr_t> find_pattern(const uint8_t* bytes, size_t n) {
    std::vector<uintptr_t> out;
    if (!n) return out;
    scan_image(exe_image(), [&](uintptr_t address, const uint8_t* data, size_t size) {
        // (a pattern across two 4 MB chunks is missed)
        for (size_t i = 0; i + n <= size;) {
            const void* hit = std::memchr(data + i, bytes[0], size - n + 1 - i);
            if (!hit) break;
            i = static_cast<const uint8_t*>(hit) - data;
            if (std::memcmp(data + i, bytes, n) == 0) out.push_back(address + i);
            ++i;
        }
    });
    return out;
}

std::vector<uintptr_t> find_in_image(const std::vector<uintptr_t>& values) {
    return find_qwords(exe_image(), {values.begin(), values.end()});
}

std::vector<std::pair<uintptr_t, uintptr_t>> find_branches(const std::vector<uintptr_t>& targets) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    const std::unordered_set<uintptr_t> wanted(targets.begin(), targets.end());
    if (wanted.empty()) return out;
    scan_image(exe_image(), [&](uintptr_t address, const uint8_t* data, size_t n) {
        // (a branch across two 4 MB chunks is missed)
        for (size_t i = 0; i + 6 <= n; ++i) {
            int32_t rel = 0;
            if (data[i] == 0xE8 || data[i] == 0xE9) {
                std::memcpy(&rel, data + i + 1, 4);
                if (const uintptr_t to = address + i + 5 + rel; wanted.count(to)) out.emplace_back(address + i, to);
            } else if (data[i] == 0xFF && (data[i + 1] == 0x15 || data[i + 1] == 0x25)) {
                std::memcpy(&rel, data + i + 2, 4);
                if (const uintptr_t slot = address + i + 6 + rel; wanted.count(slot)) out.emplace_back(address + i, slot);
            }
        }
    });
    return out;
}

std::vector<std::pair<uintptr_t, uintptr_t>> find_lea_refs(const std::vector<uintptr_t>& targets) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    const std::unordered_set<uintptr_t> wanted(targets.begin(), targets.end());
    if (wanted.empty()) return out;
    scan_image(exe_image(), [&](uintptr_t address, const uint8_t* data, size_t n) {
        // (an instruction across two 4 MB chunks is missed)
        for (size_t i = 0; i + 7 <= n; ++i) {
            if ((data[i] & 0xFB) != 0x48 || data[i + 1] != 0x8D || (data[i + 2] & 0xC7) != 0x05) continue;
            int32_t rel = 0;
            std::memcpy(&rel, data + i + 3, 4);
            if (const uintptr_t to = address + i + 7 + rel; wanted.count(to)) out.emplace_back(address + i, to);
        }
    });
    return out;
}

size_t dump_types(const TypeTable& table, const std::wstring& path, const std::string& filter) {
    std::vector<std::string> parts;
    for (size_t at = 0; at <= filter.size();) {
        const size_t comma = std::min(filter.find(',', at), filter.size());
        std::string part = filter.substr(at, comma - at);
        part.erase(0, part.find_first_not_of(' '));
        part.erase(part.find_last_not_of(' ') + 1);
        if (!part.empty()) parts.push_back(lower(part));
        at = comma + 1;
    }
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") || !out) return 0;
    std::fprintf(out, "Frostbite types: %zu, info data at +0x%X, layout %s, filter \"%s\"\n", table.types.size(),
                 table.info_offset, table.layout ? "recognised" : "unknown (raw info data)", filter.c_str());

    size_t written = 0;
    for (const TypeDesc& t : table.types) {
        const std::string key = lower(t.name);
        if (!parts.empty() &&
            std::none_of(parts.begin(), parts.end(), [&](const std::string& p) { return key.find(p) != std::string::npos; }))
            continue;
        ++written;
        if (!table.layout) {
            std::fprintf(out, "\n%s type %s info %s\n", t.name.c_str(), mem::module_offset(t.type).c_str(),
                         mem::module_offset(t.info).c_str());
            for (size_t i = 0; i < 0x70; i += 8) {
                uint64_t q = 0;
                if (mem::rd(t.info + i, q)) std::fprintf(out, "  +%02zX %016llX\n", i, (unsigned long long)q);
            }
            continue;
        }
        std::fprintf(out, "\n%s %s%s%s", kind_name(t.kind), t.name_space.empty() ? "" : (t.name_space + "::").c_str(),
                     t.name.c_str(), t.parent ? (" : " + table.type_name(t.parent)).c_str() : "");
        std::fprintf(out, "  // size 0x%X, align %u, type %s\n", t.size, t.alignment, mem::module_offset(t.type).c_str());
        for (const FieldDesc& f : t.fields) {
            if (t.kind == Enum) std::fprintf(out, "  %s = %u (%u)\n", f.name.c_str(), uint32_t(f.type), f.offset);
            else std::fprintf(out, "  +0x%-4X %s %s\n", f.offset, table.type_name(f.type).c_str(), f.name.c_str());
        }
    }
    std::fclose(out);
    return written;
}

}  // namespace gyro4all::fb
