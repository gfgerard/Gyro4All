#include "mem.hpp"

#include "hooks.hpp"
#include "log.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace gyro4all::mem {

// Only handle what a stray read can cause, and put PAGE_GUARD back. A plausible-looking pointer can land on another
// thread's stack guard page; swallowing STATUS_GUARD_PAGE_VIOLATION clears the guard, that thread can no longer grow
// its stack and the game later dies in ntdll (seen in PSButtonsUE, which shares this core: Luto, UE 5.5). Anything
// else (a real bug) still crashes normally.
int guard_filter(const _EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        if (r->NumberParameters >= 2) {
            MEMORY_BASIC_INFORMATION mbi{};
            const auto page = reinterpret_cast<void*>(r->ExceptionInformation[1] & ~uintptr_t(0xFFF));
            DWORD old = 0;
            if (VirtualQuery(page, &mbi, sizeof(mbi))) VirtualProtect(page, 1, mbi.Protect | PAGE_GUARD, &old);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

bool guarded_copy(void* dst, uintptr_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (guard_filter(GetExceptionInformation())) {
        return false;
    }
}

bool guarded_store(uintptr_t dst, const void* src, size_t n) {
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (guard_filter(GetExceptionInformation())) {
        return false;
    }
}

std::vector<Range> writable_sections(bool include_exec) {
    std::vector<Range> out;
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
            const DWORD c = s->Characteristics;
            if ((c & IMAGE_SCN_MEM_WRITE) && (include_exec || !(c & IMAGE_SCN_MEM_EXECUTE)))
                out.push_back({base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize});
        }
    }
    return out;
}

bool in_module(uintptr_t address) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &m) != 0;
}

std::string module_offset(uintptr_t address) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    char buffer[64];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &m)) {
        std::snprintf(buffer, sizeof(buffer), "heap 0x%llX", (unsigned long long)address);
        return buffer;
    }
    GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    std::snprintf(buffer, sizeof(buffer), "+0x%llX", (unsigned long long)(address - reinterpret_cast<uintptr_t>(m)));
    return narrow(name) + buffer;
}

}  // namespace gyro4all::mem
