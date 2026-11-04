#include "mem.hpp"

#include "hooks.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
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

std::vector<Range> code_sections(uintptr_t base, bool skip_writable) {
    std::vector<Range> text;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!base || !rd(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || !rd(base + dos.e_lfanew, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return text;
    const uintptr_t first =
        base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER s;
        if (!rd(first + i * sizeof s, s)) return {};
        if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && !(skip_writable && (s.Characteristics & IMAGE_SCN_MEM_WRITE)))
            text.push_back({base + s.VirtualAddress, base + s.VirtualAddress + s.Misc.VirtualSize});
    }
    return text;
}

void parse_pattern(const char* text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    for (const char* p = text; *p;) {
        if (*p == ' ') {
            ++p;
            continue;
        }
        const bool any = p[0] == '?';
        const char hex[3] = {p[0], p[1], 0};
        bytes.push_back(any ? 0 : uint8_t(std::strtoul(hex, nullptr, 16)));
        mask.push_back(any ? 0 : 0xFF);
        p += p[1] ? 2 : 1;
    }
}

std::vector<uintptr_t> find_pattern(const std::vector<Range>& sections, const uint8_t* bytes, const uint8_t* mask, size_t n) {
    std::vector<uintptr_t> found;
    if (!n) return found;
    std::vector<uint8_t> chunk(size_t(1) << 20);
    for (const Range& r : sections) {
        for (uintptr_t at = r.begin; at < r.end; at += chunk.size() - n) {
            const size_t len = size_t(std::min<uintptr_t>(chunk.size(), r.end - at));
            if (len < n || !guarded_copy(chunk.data(), at, len)) {
                if (len < chunk.size()) break;
                continue;
            }
            const uint8_t* const last = chunk.data() + len - n;
            for (const uint8_t* p = chunk.data(); p <= last &&
                                                  (p = static_cast<const uint8_t*>(std::memchr(p, bytes[0], last - p + 1)));
                 ++p) {
                size_t j = 1;
                while (j < n && (p[j] & mask[j]) == (bytes[j] & mask[j])) ++j;
                const uintptr_t hit = at + uintptr_t(p - chunk.data());
                if (j == n && (found.empty() || found.back() < hit)) found.push_back(hit);
            }
            if (len < chunk.size()) break;
        }
    }
    return found;
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
