#include "idtech_types.hpp"

#include "mem.hpp"

#include <windows.h>

#include <cstring>

namespace gyro4all::idtech {

namespace {


// The printable C string at p inside the image ("" allowed) into out, false otherwise. Guarded.
bool image_cstr(const Image& image, uintptr_t p, char* out, size_t cap) {
    if (!image.contains(p)) return false;
    __try {
        const char* s = reinterpret_cast<const char*>(p);
        for (size_t i = 0; i + 1 < cap; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0) {
                out[i] = 0;
                return true;
            }
            if (c < 32 || c >= 127) return false;
            out[i] = char(c);
        }
        return false;
    } __except (mem::guard_filter(GetExceptionInformation())) {
        return false;
    }
}

bool is_name(const char* s) {
    if (!(s[0] == '_' || (s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return false;
    for (const char* c = s; *c; ++c)
        if (!(*c == '_' || *c == ':' || *c == '<' || *c == '>' || *c == ',' || *c == ' ' || *c == '*' ||
              (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')))
            return false;
    return true;
}

// A classVariableInfo_t at p: its name, offset and size.
bool variable_at(const Image& image, uintptr_t p, char* name, size_t cap, uint32_t& offset, uint32_t& size) {
    uint64_t q[4];
    if (!mem::guarded_copy(q, p, sizeof q)) return false;
    char type[256];
    if (!image_cstr(image, q[0], type, sizeof type) || !type[0]) return false;
    if (!image_cstr(image, q[2], name, cap) || !name[0]) return false;
    offset = uint32_t(q[3]);
    size = uint32_t(q[3] >> 32);
    return true;
}

}  // namespace

bool Image::load() {
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    end = base + nt->OptionalHeader.SizeOfImage;
    // By name; a section missing by name (renamed by a DRM wrapper or packer: random names) is the largest section of
    // its kind: executable (.text), writable (.data), other initialized data (.rdata).
    Section by_kind[3];
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        char name[9] = {};
        std::memcpy(name, s->Name, 8);
        Section sec{base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize, 0};
        sec.init_end = base + s->VirtualAddress + (s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData
                                                                                          : s->Misc.VirtualSize);
        if (!std::strcmp(name, ".text")) text = sec;
        else if (!std::strcmp(name, ".rdata")) rdata = sec;
        else if (!std::strcmp(name, ".data")) data = sec;
        const DWORD c = s->Characteristics;
        const int kind = (c & IMAGE_SCN_MEM_EXECUTE)              ? 0
                         : (c & IMAGE_SCN_MEM_WRITE)              ? 1
                         : (c & IMAGE_SCN_CNT_INITIALIZED_DATA)   ? 2
                                                                  : -1;
        if (kind >= 0 && sec.end - sec.begin > by_kind[kind].end - by_kind[kind].begin) by_kind[kind] = sec;
    }
    if (!text.begin) text = by_kind[0];
    if (!data.begin) data = by_kind[1];
    if (!rdata.begin) rdata = by_kind[2];
    return text.begin && rdata.begin && data.begin;
}

void add_class(TypeTable& t, const char* name, const char* super, uint64_t size, uintptr_t variables) {
    t.classes_.try_emplace(name, ClassInfo{super, size, variables});
}

// One layout's classes: headers of `words` qwords { name, superName, ..., size at size_at, ..., 0, variables }.
size_t TypeTable::scan(const Image& image, int words, int size_at) {
    classes_.clear();
    const Section* sections[] = {&image.rdata, &image.data};
    for (const Section* sec : sections) {
        for (uintptr_t p = sec->begin; p + 8 * words <= sec->init_end; p += 8) {
            uint64_t q[6];
            if (!mem::guarded_copy(q, p, 8 * size_t(words))) continue;
            const uint64_t size = q[size_at], zero = q[words - 2], variables = q[words - 1];
            if (zero != 0 || size == 0 || size >= 0x10000000 || !image.contains(q[0]) || !image.contains(q[1])) continue;
            if (variables && !image.contains(variables)) continue;
            char name[256], super[256], var[256];
            if (!image_cstr(image, q[0], name, sizeof name) || !is_name(name)) continue;
            if (!image_cstr(image, q[1], super, sizeof super)) continue;
            uint32_t offset = 0, field_size = 0;
            if (variables && !variable_at(image, variables, var, sizeof var, offset, field_size)) continue;
            add_class(*this, name, super, size, variables);
        }
    }
    return classes_.size();
}

size_t TypeTable::load(const Image& image) {
    image_ = &image;
    const size_t old_layout = scan(image, 5, 2);
    auto old_classes = std::move(classes_);
    const size_t new_layout = scan(image, 6, 3);
    if (old_layout >= new_layout) {
        classes_ = std::move(old_classes);
        stride_ = 0x48;
    } else {
        stride_ = 0x58;
    }
    return classes_.size();
}

const ClassInfo* TypeTable::find(const std::string& cls) const {
    const auto it = classes_.find(cls);
    return it == classes_.end() ? nullptr : &it->second;
}

int64_t TypeTable::field(const std::string& cls, const std::string& name, uint32_t* size_out) const {
    std::string c = cls;
    for (int depth = 0; depth < 32 && !c.empty(); ++depth) {
        const ClassInfo* info = find(c);
        if (!info) return -1;
        char var[256];
        uint32_t offset = 0, size = 0;
        for (uintptr_t p = info->variables; p && variable_at(*image_, p, var, sizeof var, offset, size); p += stride_)
            if (name == var) {
                if (size_out) *size_out = size;
                return offset;
            }
        c = info->super;
    }
    return -1;
}

uintptr_t find_qword(uintptr_t begin, uintptr_t end, uint64_t value) {
    // Page by page, so an unreadable page only skips itself.
    for (uintptr_t page = begin & ~uintptr_t(0xFFF); page < end; page += 0x1000) {
        const uintptr_t from = page < begin ? begin : page, to = page + 0x1000 < end ? page + 0x1000 : end;
        __try {
            for (uintptr_t p = (from + 7) & ~uintptr_t(7); p + 8 <= to; p += 8)
                if (*reinterpret_cast<const uint64_t*>(p) == value) return p;
        } __except (mem::guard_filter(GetExceptionInformation())) {
        }
    }
    return 0;
}

namespace {

// The 16-aligned qwords equal to value in [begin, end), appended to out up to max. Guarded per region.
size_t scan_region(uintptr_t begin, uintptr_t end, uint64_t value, uintptr_t* out, size_t n, size_t max) {
    __try {
        for (uintptr_t p = begin; p + 8 <= end && n < max; p += 16)
            if (*reinterpret_cast<const uint64_t*>(p) == value) out[n++] = p;
    } __except (mem::guard_filter(GetExceptionInformation())) {
    }
    return n;
}

}  // namespace

size_t heap_find(uint64_t value, uintptr_t* out, size_t max, uint64_t* bytes) {
    size_t n = 0;
    uint64_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t p = 0x10000; p < 0x7FFFFFFF0000ull && n < max; p = uintptr_t(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi)) break;
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY;
        // Write-combined and uncached pages are GPU memory mapped into the process (Vulkan): very slow to read and never
        // engine objects.
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & rw) ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)))
            continue;
        const uintptr_t b = uintptr_t(mbi.BaseAddress);
        n = scan_region(b, b + mbi.RegionSize, value, out, n, max);
        scanned += mbi.RegionSize;
    }
    if (bytes) *bytes = scanned;
    return n;
}

uintptr_t rtti_vtable(const Image& image, const char* decorated) {
    // Type descriptor: { vftable of type_info, spare, char name[] }; the name follows 16 bytes in.
    const size_t n = std::strlen(decorated) + 1;
    uintptr_t td = 0;
    for (uintptr_t p = image.data.begin; p + n <= image.data.init_end && !td; p += 8) {
        char buf[64];
        if (n > sizeof buf || !mem::guarded_copy(buf, p + 16, n)) continue;
        if (!std::memcmp(buf, decorated, n)) td = p;
    }
    if (!td) return 0;
    // Complete object locator in .rdata: { signature 1, offset 0, cdOffset, typeDescriptor rva, classDescriptor rva,
    // self rva }, then the vtable's slot -1 points to it.
    const uint32_t td_rva = uint32_t(td - image.base);
    for (uintptr_t p = image.rdata.begin; p + 24 <= image.rdata.init_end; p += 4) {
        uint32_t col[6];
        if (!mem::guarded_copy(col, p, sizeof col)) continue;
        if (col[0] != 1 || col[1] != 0 || col[3] != td_rva || col[5] != uint32_t(p - image.base)) continue;
        if (const uintptr_t slot = find_qword(image.rdata.begin, image.rdata.init_end, p)) return slot + 8;
    }
    return 0;
}

std::string rtti_name(const Image& image, uintptr_t p) {
    const uintptr_t vt = mem::rdp(p);
    if (!image.contains(vt)) return {};
    const uintptr_t col = mem::rdp(vt - 8, 4);
    if (!image.contains(col)) return {};
    uint32_t td_rva = 0;
    if (!mem::rd(col + 12, td_rva)) return {};
    char name[128];
    if (!image_cstr(image, image.base + td_rva + 16, name, sizeof name)) return {};
    return name;
}

}  // namespace gyro4all::idtech
