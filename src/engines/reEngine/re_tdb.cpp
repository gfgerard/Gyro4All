#include "re_tdb.hpp"

#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace gyro4all::re {

namespace {

constexpr uint32_t kTypeImplSize = 0x30;

// Where the header keeps its counts and table pointers.
struct HeaderLayout {
    uint32_t num_types, num_strings, types, types_impl, fields, fields_impl, strings;
};
// TDB 69/70/71 (tdb69/tdb70::TDB; 70 adds a pointer after initData; 71, RE4, keeps 70's header), 81 (tdb81::TDB: the counter added in TDB 74 moves
// the counts, a padding u32 keeps the pointers where 83 has them) and 83 (tdb83::TDB: one more counter from TDB 82).
constexpr HeaderLayout kHeader69{0x0C, 0x50, 0x60, 0x68, 0x80, 0x88, 0xC8};
constexpr HeaderLayout kHeader70{0x0C, 0x50, 0x60, 0x68, 0x80, 0x88, 0xD0};
constexpr HeaderLayout kHeader81{0x08, 0x54, 0x68, 0x70, 0x88, 0x90, 0xD8};
constexpr HeaderLayout kHeader83{0x08, 0x58, 0x68, 0x70, 0x88, 0x90, 0xD8};

const HeaderLayout* header_layout(uint32_t version) {
    switch (version) {
    case 69: return &kHeader69;
    case 70:
    case 71: return &kHeader70;
    case 81: return &kHeader81;
    case 83: return &kHeader83;
    default: return nullptr;
    }
}

uint64_t bits(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

// A printable C string of at most max chars, "" if unreadable.
std::string read_cstr(uintptr_t address, size_t max = 128) {
    std::string out;
    char block[16];
    while (out.size() < max) {
        if (!mem::guarded_copy(block, address + out.size(), sizeof(block))) return {};
        for (char c : block) {
            if (c == 0) return out;
            if (c < 0x20 || c > 0x7E) return {};
            out.push_back(c);
        }
    }
    return {};
}

std::string pool_string(const Tdb& tdb, uint32_t offset) { return read_cstr(tdb.strings + (offset & tdb.string_mask)); }

uintptr_t type_definition(const Tdb& tdb, uint32_t index) { return tdb.types + uint64_t(index) * tdb.type_size; }

// A type definition's first qword: index, parent, declaring type (18 bits each, or 19 from TDB 71).
uint64_t type_head(const Tdb& tdb, uint32_t index) {
    uint64_t head = 0;
    return index < tdb.num_types && mem::rd(type_definition(tdb, index), head) ? head : 0;
}

uint32_t parent_of(const Tdb& tdb, uint64_t head) {
    return uint32_t(tdb.wide ? bits(head, 19, 19) : bits(head, 18, 18));
}
uint32_t declaring_of(const Tdb& tdb, uint64_t head) {
    return uint32_t(tdb.wide ? bits(head, 38, 19) : bits(head, 36, 18));
}

// The type's impl record (names, member counts).
uintptr_t type_impl(const Tdb& tdb, uint32_t index) {
    uint64_t second = 0;
    if (!mem::rd(type_definition(tdb, index) + 8, second)) return 0;
    return tdb.types_impl + bits(second, tdb.wide ? 38 : 36, 18) * kTypeImplSize;
}

// Name and namespace offsets in the string pool: two int32 (TDB 69/70/81), or 28-bit fields of one qword (83).
bool type_names(const Tdb& tdb, uint32_t index, uint32_t names[2]) {
    const uintptr_t impl = type_impl(tdb, index);
    if (!impl) return false;
    if (tdb.version >= 83) {
        uint64_t packed = 0;
        if (!mem::rd(impl, packed)) return false;
        names[0] = uint32_t(bits(packed, 0, 28));
        names[1] = uint32_t(bits(packed, 28, 28));
        return true;
    }
    return mem::guarded_copy(names, impl, 2 * sizeof(uint32_t));
}

// A type's own member fields: the first one's index and how many.
bool member_fields(const Tdb& tdb, uint32_t index, uint32_t& first, uint32_t& count) {
    const uintptr_t td = type_definition(tdb, index);
    const uintptr_t impl = type_impl(tdb, index);
    if (!impl) return false;
    if (tdb.wide) {
        // RETypeDefVersion71/74/83 (same fields; 71 is 0x48 bytes): default_ctor:22, member_method:22, member_field:20 at 0x20; RETypeImpl: num_member_fields
        // bits 33..56 of the qword at 0x10.
        uint64_t packed = 0, counts = 0;
        if (!mem::rd(td + 0x20, packed) || !mem::rd(impl + 0x10, counts)) return false;
        first = uint32_t(bits(packed, 44, 20));
        count = uint32_t(bits(counts, 33, 24));
        return true;
    }
    int32_t n = 0;
    if (!mem::rd(td + 0x2C, first) || !mem::rd(impl + 0x14, n)) return false;
    count = uint32_t(std::max(n, 0));
    return true;
}

// One member field: its name offset, flags (static, literal...), offset from the object's field base and type.
bool field_info(const Tdb& tdb, uint32_t field_index, uint32_t& name, uint16_t& flags, int32_t& offset,
                uint32_t* type = nullptr) {
    uint64_t field = 0;
    if (!mem::rd(tdb.fields + uint64_t(field_index) * 8, field)) return false;
    struct {
        uint16_t attributes, flags;
        uint32_t a, b;
    } impl{};
    if (tdb.wide) {
        // REField: declaring:19, impl_id:19, field_typeid:19. REFieldImpl: u16 unk:1 flags:15, u32 offset:26,
        // u32 name_offset:28.
        if (!mem::rd(tdb.fields_impl + bits(field, 19, 19) * 12, impl)) return false;
        flags = uint16_t(impl.flags >> 1);
        offset = int32_t(bits(impl.a, 0, 26));
        name = uint32_t(bits(impl.b, 0, 28));
        if (type) *type = uint32_t(bits(field, 38, 19));
        return true;
    }
    // REField: declaring:18, impl_id:20, offset:26. REFieldImpl: u16 flags, u32 field_typeid:18, u32 name_offset:30.
    if (!mem::rd(tdb.fields_impl + bits(field, 18, 20) * 12, impl)) return false;
    flags = impl.flags;
    offset = int32_t(bits(field, 38, 26));
    name = uint32_t(bits(impl.b, 0, 30));
    if (type) *type = uint32_t(bits(impl.a, 0, 18));
    return true;
}

using SingletonGetter = void (*)(uintptr_t type, void** out, void* unused);

// No C++ objects here: __try needs a frame without unwinding.
uintptr_t call_getter(SingletonGetter getter, uintptr_t type) {
    void* out = nullptr;
    __try {
        getter(type, &out, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(out);
}

}  // namespace

bool tdb_supported(uint32_t version) { return header_layout(version) != nullptr; }

uintptr_t find_tdb_header(uint32_t& version) {
    static const char kMagic[4] = {'T', 'D', 'B', 0};
    std::vector<char> buffer(1 << 20);
    for (const mem::Range& r : mem::writable_sections()) {
        for (uintptr_t at = r.begin; at < r.end; at += buffer.size() - 8) {
            const size_t n = std::min<size_t>(buffer.size(), r.end - at);
            if (n < 8 || !mem::guarded_copy(buffer.data(), at, n)) continue;
            for (auto it = buffer.begin(); (it = std::search(it, buffer.begin() + n - 8, kMagic, kMagic + 4)) !=
                                           buffer.begin() + n - 8;
                 ++it) {
                const size_t offset = size_t(it - buffer.begin());
                uint32_t v = 0;
                std::memcpy(&v, buffer.data() + offset + 4, sizeof(v));
                if ((at + offset) % 8 == 0 && v >= 49 && v <= 120) {
                    version = v;
                    return at + offset;
                }
            }
        }
    }
    return 0;
}

bool read_tdb(uintptr_t header, Tdb& out) {
    uint32_t version = 0;
    if (!mem::rd(header + 4, version)) return false;
    const HeaderLayout* h = header_layout(version);
    if (!h) return false;
    // On disk the tables are offsets from the header; the game's startup turns them into pointers. The header's
    // "initialized" field is no use: RE7 leaves it at 0. So: ready once the types table is a pointer past it.
    uint64_t types_raw = 0;
    if (!mem::rd(header + h->types, types_raw) || types_raw <= header) return false;
    Tdb t;
    t.header = header;
    t.version = version;
    t.wide = version >= 71;
    // RETypeDefVersion71 is RETypeDefVersion74 without its last pointer (checked offline on RE4: types table size /
    // count = 0x48).
    t.type_size = version == 71 ? 0x48 : 0x50;
    uint32_t num_strings = 0;
    if (!mem::rd(header + h->num_types, t.num_types) || !mem::rd(header + h->num_strings, num_strings)) return false;
    t.types = mem::rdp(header + h->types);
    t.types_impl = mem::rdp(header + h->types_impl);
    t.strings = mem::rdp(header + h->strings, 1);
    uint32_t mask = 1;
    while (mask < num_strings) mask <<= 1;
    t.string_mask = mask - 1;
    t.fields = mem::rdp(header + h->fields);
    t.fields_impl = mem::rdp(header + h->fields_impl, 4);
    if (!t.types || !t.types_impl || !t.strings || !t.fields || !t.fields_impl || t.num_types == 0 ||
        t.num_types >= (1u << (t.wide ? 19 : 18)))
        return false;
    out = t;
    return true;
}

uint32_t find_type(const Tdb& tdb, const char* name_space, const char* name) {
    for (uint32_t i = 1; i < tdb.num_types; ++i) {
        uint32_t names[2];
        if (!type_names(tdb, i, names)) continue;
        if (pool_string(tdb, names[0]) == name && pool_string(tdb, names[1]) == name_space) return i;
    }
    return 0;
}

std::string type_name(const Tdb& tdb, uint32_t index) {
    std::string out;
    for (int depth = 0; index && depth < 8; ++depth) {
        uint32_t names[2];
        if (!type_names(tdb, index, names)) return {};
        const std::string name = pool_string(tdb, names[0]);
        out = out.empty() ? name : name + "." + out;
        const uint32_t declaring = declaring_of(tdb, type_head(tdb, index));
        if (!declaring || declaring == index) {
            const std::string ns = pool_string(tdb, names[1]);
            return ns.empty() ? out : ns + "." + out;
        }
        index = declaring;
    }
    return out;
}

uint32_t parent_type(const Tdb& tdb, uint32_t index) { return parent_of(tdb, type_head(tdb, index)); }

bool is_a(const Tdb& tdb, uint32_t index, uint32_t base_index) {
    for (int depth = 0; index && depth < 64; ++depth) {
        if (index == base_index) return true;
        const uint32_t parent = parent_type(tdb, index);
        if (parent == index) break;
        index = parent;
    }
    return false;
}

int32_t field_offset(const Tdb& tdb, uint32_t index, const char* name) {
    constexpr uint16_t kStatic = 0x10, kLiteral = 0x40;
    for (int depth = 0; index && depth < 64; ++depth) {
        uint32_t first = 0, count = 0;
        if (!member_fields(tdb, index, first, count)) return -1;
        for (uint32_t k = 0; k < count && k < 4096; ++k) {
            uint32_t name_offset = 0;
            uint16_t flags = 0;
            int32_t offset = 0;
            if (!field_info(tdb, first + k, name_offset, flags, offset)) return -1;
            if (flags & (kStatic | kLiteral)) continue;
            if (pool_string(tdb, name_offset) == name) return offset;
        }
        const uint32_t parent = parent_type(tdb, index);
        if (parent == index) break;
        index = parent;
    }
    return -1;
}

std::vector<FieldDesc> own_fields(const Tdb& tdb, uint32_t index) {
    constexpr uint16_t kStatic = 0x10, kLiteral = 0x40;
    std::vector<FieldDesc> out;
    uint32_t first = 0, count = 0;
    if (!member_fields(tdb, index, first, count)) return out;
    for (uint32_t k = 0; k < count && k < 4096; ++k) {
        uint32_t name_offset = 0, type = 0;
        uint16_t flags = 0;
        int32_t offset = 0;
        if (!field_info(tdb, first + k, name_offset, flags, offset, &type)) break;
        out.push_back({pool_string(tdb, name_offset), offset, type, (flags & (kStatic | kLiteral)) != 0});
    }
    return out;
}

uint32_t object_type(const Tdb& tdb, uintptr_t obj) {
    const uintptr_t info = obj ? mem::rdp(obj) : 0;
    const uintptr_t definition = info ? mem::rdp(info) : 0;
    if (definition < tdb.types || definition >= tdb.types + uint64_t(tdb.num_types) * tdb.type_size) return 0;
    const uintptr_t offset = definition - tdb.types;
    return offset % tdb.type_size == 0 ? uint32_t(offset / tdb.type_size) : 0;
}

int32_t field_base(uintptr_t obj) {
    const uintptr_t info = obj ? mem::rdp(obj) : 0;
    int32_t base = -1;
    return info && mem::rd(info - 8, base) && base >= 0 && base < 0x10000 ? base : -1;
}

std::vector<uintptr_t> singleton_slots() {
    std::vector<uintptr_t> out;
    const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    const uintptr_t image_end = exe + nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    // lea r64, [rip+disp32] (48 8D modrm with rm=101) ; mov rax, 0x8000000000000000
    static const uint8_t kMov[10] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0x80};
    std::vector<uint8_t> buffer(1 << 20);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t begin = exe + section->VirtualAddress, end = begin + section->Misc.VirtualSize;
        for (uintptr_t at = begin; at < end; at += buffer.size() - 32) {
            const size_t n = std::min<size_t>(buffer.size(), end - at);
            if (n < 17 || !mem::guarded_copy(buffer.data(), at, n)) continue;
            for (size_t i = 0; i + 17 <= n; ++i) {
                if (buffer[i] != 0x48 || buffer[i + 1] != 0x8D || (buffer[i + 2] & 0xC7) != 0x05) continue;
                if (std::memcmp(&buffer[i + 7], kMov, sizeof(kMov)) != 0) continue;
                int32_t disp;
                std::memcpy(&disp, &buffer[i + 3], sizeof(disp));
                const uintptr_t slot = at + i + 7 + disp;
                if (slot >= exe && slot < image_end && slot % 8 == 0 &&
                    std::find(out.begin(), out.end(), slot) == out.end())
                    out.push_back(slot);
            }
        }
    }
    return out;
}

std::vector<uintptr_t> managed_object_slots(const Tdb& tdb) {
    std::vector<uintptr_t> out;
    std::vector<uintptr_t> buffer(1 << 17);
    // Protected exes may keep their data in writable+executable sections: those too when the others have none.
    for (const bool include_exec : {false, true}) {
        for (const mem::Range& r : mem::writable_sections(include_exec)) {
            for (uintptr_t at = r.begin & ~uintptr_t(7); at + 8 <= r.end; at += buffer.size() * 8) {
                const size_t n = std::min<size_t>(buffer.size(), (r.end - at) / 8);
                if (!mem::guarded_copy(buffer.data(), at, n * 8)) continue;
                for (size_t i = 0; i < n; ++i)
                    if (mem::plausible(buffer[i], 16) && object_type(tdb, buffer[i])) out.push_back(at + i * 8);
            }
        }
        if (!out.empty()) break;
        out.clear();
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uintptr_t runtime_type(const Tdb& tdb, uint32_t index) {
    // RETypeCLR* at 0x40 (RETypeDefVersion69) or 0x38 (RETypeDefVersion71/74/83).
    return mem::rdp(type_definition(tdb, index) + (tdb.wide ? 0x38 : 0x40));
}

uintptr_t singleton_instance(uintptr_t type) {
    // The runtime type's second virtual function returns the singleton (REFramework get_singleton_instance).
    const uintptr_t vtable = mem::rdp(type);
    const uintptr_t getter = vtable ? mem::rdp(vtable + 8, 1) : 0;
    if (!getter || !mem::in_module(getter)) return 0;
    const uintptr_t instance = call_getter(reinterpret_cast<SingletonGetter>(getter), type);
    return mem::plausible(instance) ? instance : 0;
}

std::vector<AppEntry> application_entries(const Tdb& tdb, uintptr_t application) {
    // An entry is 0xD0 bytes with its name at +0x18 and priority (via.ModuleEntry value) at +0x20 below TDB 74,
    // 0xC8 bytes with them at +0x10 / +0x18 from TDB 74 (REFramework Application::Function). Found by shape, like
    // REFramework's fallback: somewhere in the object, a run of entries whose function is in a module and whose name
    // is a readable string, including the rendering steps every RE Engine game has.
    const bool v74 = tdb.version >= 74;
    const uint32_t stride = v74 ? 0xC8 : 0xD0, name_at = v74 ? 0x10 : 0x18, priority_at = v74 ? 0x18 : 0x20;
    for (uintptr_t offset = 0x100; offset < 0x1000; offset += 8) {
        std::vector<AppEntry> entries;
        bool begin = false, end = false;
        for (uint32_t j = 0; j < 1024; ++j) {
            const uintptr_t e = application + offset + uint64_t(j) * stride;
            const uintptr_t function = mem::rdp(e + 8, 1);
            const uintptr_t name_ptr = mem::rdp(e + name_at, 1);
            if (!function || !mem::in_module(function) || !name_ptr) break;
            AppEntry entry;
            entry.address = e;
            entry.name = read_cstr(name_ptr, 64);
            if (entry.name.empty() || !mem::rd(e + priority_at, entry.priority)) break;
            begin |= entry.name == "BeginRendering";
            end |= entry.name == "EndRendering";
            entries.push_back(std::move(entry));
        }
        if (begin && end) return entries;
    }
    return {};
}

}  // namespace gyro4all::re
