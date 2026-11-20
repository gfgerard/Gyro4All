#include "rtti.hpp"

#include "mem.hpp"

#include <windows.h>

#include <cstring>

namespace gyro4all::rtti {

namespace {

// Every aligned position of `what` in the image's data sections.
template <class F>
void find_in_data(const Image& im, const void* what, size_t n, size_t align, F&& found) {
    for (const auto& [b, e] : im.data)
        for (uintptr_t p = b; p + n <= e; p += align)
            if (std::memcmp(reinterpret_cast<const void*>(p), what, n) == 0 && !found(p)) return;
}

}  // namespace

Image exe_image() {
    Image im;
    im.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(im.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(im.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            im.data.push_back({im.base + s->VirtualAddress, im.base + s->VirtualAddress + s->Misc.VirtualSize});
    return im;
}

uintptr_t primary_vtable(const Image& im, const char* rtti_name) {
    const size_t len = std::strlen(rtti_name) + 1;
    uintptr_t type = 0;  // the TypeDescriptor: {vtable, spare, name}
    find_in_data(im, rtti_name, len, 1, [&](uintptr_t p) {
        type = p - 0x10;
        return false;
    });
    if (!type) return 0;
    const uint32_t type_rva = uint32_t(type - im.base);
    uintptr_t vtable = 0;
    find_in_data(im, &type_rva, 4, 4, [&](uintptr_t p) {
        const auto* col = reinterpret_cast<const uint32_t*>(p - 0xC);  // {signature 1, offset, cd offset, type, ...}
        if (col[0] != 1 || col[1] != 0) return true;
        const uint64_t col_va = uint64_t(p - 0xC);
        find_in_data(im, &col_va, 8, 8, [&](uintptr_t q) {
            vtable = q + 8;
            return false;
        });
        return vtable == 0;
    });
    return vtable;
}

std::string class_name(uintptr_t obj) {
    uintptr_t vt = 0, col = 0;
    uint32_t sig_type[4] = {};
    if (!mem::rd(obj, vt) || !mem::rd(vt - 8, col) || !mem::rd(col, sig_type) || sig_type[0] != 1) return "?";
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char name[96] = {};
    if (!mem::guarded_copy(name, base + sig_type[3] + 0x10, sizeof name - 1)) return "?";
    return name;
}

void* patch_slot(uintptr_t vtable, int slot, void* hook) {
    auto* at = reinterpret_cast<void**>(vtable + uintptr_t(slot) * 8);
    void* original = nullptr;
    if (!vtable || !mem::rd(reinterpret_cast<uintptr_t>(at), original) ||
        !mem::in_module(reinterpret_cast<uintptr_t>(original)))
        return nullptr;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    *at = hook;
    VirtualProtect(at, sizeof(void*), old, &old);
    return original;
}

}  // namespace gyro4all::rtti
