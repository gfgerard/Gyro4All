#pragma once
// Guarded access to the game's memory, for engine modules that read engine data structures at runtime: a bad
// pointer returns false (or 0) instead of crashing the game. Plus the game modules' writable sections, where
// engine globals live.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct _EXCEPTION_POINTERS;

namespace gyro4all::mem {

// __except filter for engine memory reads: handles access violations (and restores a hit PAGE_GUARD), lets
// anything else crash normally. Use as __except (mem::guard_filter(GetExceptionInformation())).
int guard_filter(const _EXCEPTION_POINTERS* info);
bool guarded_copy(void* dst, uintptr_t src, size_t n);
bool guarded_store(uintptr_t dst, const void* src, size_t n);

// A user-mode, aligned address (not a proof it is mapped: reads still go through guarded_copy).
inline bool plausible(uintptr_t p, uintptr_t align = 8) {
    return p >= 0x10000 && p < 0x7FFFFFFF0000ull && (p & (align - 1)) == 0;
}

template <class T>
bool rd(uintptr_t address, T& out) {
    return guarded_copy(&out, address, sizeof(T));
}

// A pointer read from address, 0 if unreadable or implausible.
inline uintptr_t rdp(uintptr_t address, uintptr_t align = 8) {
    uintptr_t p = 0;
    return rd(address, p) && plausible(p, align) ? p : 0;
}

struct Range {
    uintptr_t begin, end;
};

// Writable sections of every game module (hooks::game_modules: the exe plus a modular build's own DLLs).
// Protected exes may merge data into writable+executable sections; those are only included with include_exec
// (slower to scan: they can be hundreds of MB).
std::vector<Range> writable_sections(bool include_exec = false);
bool in_module(uintptr_t address);         // address lies in a loaded module (e.g. a vtable)
std::string module_offset(uintptr_t address);  // "Game.exe+0x1234", or "heap 0x..." outside modules

}  // namespace gyro4all::mem
