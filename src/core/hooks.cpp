#include "hooks.hpp"

#include "log.hpp"

#include <MinHook.h>
#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>

namespace gyro4all::hooks {
namespace {

struct Hook {
    const char* dll;  // null = any
    const char* function;
    void* replacement;
    void** original;
    bool warned = false;
    std::string from;  // DLL name of the first import patched (e.g. "XINPUT1_4.dll")
    WORD ordinal = 0;  // nonzero: match imports by this ordinal (dll required) instead of by name
    bool pad_library = false;  // also patched in Sony's pad library DLL (libScePad*.dll)
};

std::recursive_mutex g_mutex;
std::vector<Hook> g_hooks;
std::set<std::pair<HMODULE, size_t>> g_done;  // (module, hook index) already patched
// Every import slot patched, to notice when something else writes it back (verify()).
struct Patched {
    ULONGLONG* slot;
    void* replacement;
    const char* function;
    std::string dll;
    bool reported = false;
};
std::vector<Patched> g_patched;
std::wstring g_prefix;                        // lower-case prefix of the game's own DLLs (Unreal: "<project>-")
// The exe's import table is a protector stub (no name tables; the real imports are resolved later by
// the protector, after DllMain), so hooks on a named DLL go inline on that DLL's export instead.
bool g_inline = false;

using LoadLibraryW_t = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryExA_t = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
LoadLibraryW_t g_load_w = nullptr;
LoadLibraryA_t g_load_a = nullptr;
LoadLibraryExW_t g_load_ex_w = nullptr;
LoadLibraryExA_t g_load_ex_a = nullptr;

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

bool is_game_module(HMODULE m) {
    if (m == GetModuleHandleW(nullptr)) return true;
    if (g_prefix.empty()) return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    std::wstring name = path;
    name = lower(name.substr(name.find_last_of(L"\\/") + 1));
    return name.rfind(g_prefix, 0) == 0;
}

// Sony's PC pad library shipped as its own DLL (libScePad.dll): it opens the controller itself, so the hooks that must
// see that are patched in it too. Not a game module otherwise (no memory scans, no import checks).
bool is_pad_library(HMODULE m) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    std::wstring name = path;
    name = lower(name.substr(name.find_last_of(L"\\/") + 1));
    return name.rfind(L"libscepad", 0) == 0;
}

// API-set names (api-ms-win-*, ext-ms-win-*) stand for system DLLs: GDK/OneCore builds import user32
// functions as e.g. ext-ms-win-ntuser-message-l1-1-0.dll!PeekMessageW. The function name decides then.
bool dll_matches(const char* imported, const char* wanted) {
    if (!wanted || _stricmp(imported, wanted) == 0) return true;
    return _strnicmp(imported, "api-ms-", 7) == 0 || _strnicmp(imported, "ext-ms-", 7) == 0;
}

// Where an import slot points, as "module+0xoffset" (diagnostics).
std::string describe(const void* address) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &owner))
        return "unmapped/dynamic code";
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(owner, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L"\\/") + 1);
    char offset[32];
    std::snprintf(offset, sizeof offset, "+0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(owner)));
    return narrow(name) + offset;
}

// [rva, rva + size) lies inside the module's image. The import walks check every RVA they follow: a protector's tables
// may still be encrypted while it loads the real imports (EA's .ooa stub loads the proxy from there, Mass Effect LE).
bool in_image(uintptr_t base, uint64_t rva, uint64_t size) {
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uint64_t image = nt->OptionalHeader.SizeOfImage;
    return rva && rva < image && size <= image - rva;
}

// The module has a section of this name (at most 8 characters).
bool has_section(HMODULE module, const char* name) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        if (std::strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) return true;
    return false;
}

// The resolved system function a hook with a named dll replaces (for import tables without names).
void* system_target(const Hook& hook) {
    if (!hook.dll) return nullptr;
    HMODULE m = GetModuleHandleA(hook.dll);
    return m ? reinterpret_cast<void*>(GetProcAddress(m, hook.function)) : nullptr;
}

// Patch the slots of one imported DLL (regular or delay-load table). Returns true if a slot now points to the hook.
// A delay-load slot that hasn't been called yet points to the linker's resolver stub inside the module itself, not
// to the real function: it is patched (so the stub never runs) but doesn't count as the original.
bool patch_slots(HMODULE module, const char* dll, const IMAGE_THUNK_DATA64* names, IMAGE_THUNK_DATA64* slots, Hook& hook,
                 bool delay) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    bool patched = false;
    if (hook.ordinal && _stricmp(dll, hook.dll) != 0) return false;  // ordinals only mean something for that exact DLL
    void* const target = system_target(hook);
    for (; in_image(base, reinterpret_cast<uintptr_t>(slots) - base, sizeof *slots) && slots->u1.Function;
         ++slots, names = names ? names + 1 : nullptr) {
        if (names && !in_image(base, reinterpret_cast<uintptr_t>(names) - base, sizeof *names)) names = nullptr;
        // Match by name; without a name table (stripped by some linkers/protectors), by resolved address. Ordinal hooks
        // match ordinal imports only (the Windows SDK's xinput.lib imports XInput by ordinal).
        if (hook.ordinal) {
            if (!names || !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) || IMAGE_ORDINAL64(names->u1.Ordinal) != hook.ordinal) continue;
        } else if (names && names->u1.AddressOfData && !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
            if (!in_image(base, names->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(by_name->Name, hook.function) != 0) continue;
        } else if (!target || reinterpret_cast<void*>(slots->u1.Function) != target) {
            continue;
        }
        void* current = reinterpret_cast<void*>(slots->u1.Function);
        if (current == hook.replacement) {
            patched = true;
            continue;
        }
        const bool unresolved = delay && slots->u1.Function >= base && slots->u1.Function < end;
        if (unresolved) {
            // The DLL may not be loaded yet (no LoadLibrary here: this can run in DllMain); then the caller resolves the
            // original itself from source_dll().
            if (!*hook.original)
                if (HMODULE loaded = GetModuleHandleA(dll))
                    *hook.original = reinterpret_cast<void*>(
                        GetProcAddress(loaded, hook.ordinal ? MAKEINTRESOURCEA(hook.ordinal) : hook.function));
        } else if (!*hook.original) {
            *hook.original = current;
        } else if (current != *hook.original) {
            if (!hook.warned)
                log("hooks: %s in module %p resolves elsewhere (%p); such modules are left alone", hook.function,
                    static_cast<void*>(module), current);
            hook.warned = true;
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
        slots->u1.Function = reinterpret_cast<ULONGLONG>(hook.replacement);
        VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
        g_patched.push_back({&slots->u1.Function, hook.replacement, hook.function, dll});
        if (hook.from.empty()) hook.from = dll;
        if (delay || hook.ordinal)
            log("hooks: %s!%s%s patched in the %s table of module %p%s", dll, hook.function,
                hook.ordinal ? (" (ordinal " + std::to_string(hook.ordinal) + ")").c_str() : "", delay ? "delay-load" : "import",
                static_cast<void*>(module), unresolved ? " (not called yet)" : "");
        patched = true;
    }
    return patched;
}

// Patch one hook in one module's import tables (regular and delay-load). Returns true if a slot now points to the hook.
bool patch_module(HMODULE module, Hook& hook) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    bool patched = false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress) {
        for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
             in_image(base, reinterpret_cast<uintptr_t>(imp) - base, sizeof *imp) && imp->Name; ++imp) {
            if (!in_image(base, imp->Name, 1) || !in_image(base, imp->FirstThunk, sizeof(IMAGE_THUNK_DATA64))) continue;
            const char* dll = reinterpret_cast<const char*>(base + imp->Name);
            if (!dll_matches(dll, hook.dll)) continue;
            auto* names = imp->OriginalFirstThunk ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk) : nullptr;
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
            patched = patch_slots(module, dll, names, slots, hook, false) || patched;
        }
    }
    // Delay-load imports (UE5 delay-loads XINPUT1_4.dll, for example): same layout, RVAs in a different descriptor.
    const auto& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay.VirtualAddress) {
        for (auto* imp = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay.VirtualAddress);
             in_image(base, reinterpret_cast<uintptr_t>(imp) - base, sizeof *imp) && imp->DllNameRVA; ++imp) {
            if (!imp->Attributes.RvaBased || !imp->ImportAddressTableRVA) continue;  // pre-VC7 layout (absolute VAs)
            if (!in_image(base, imp->DllNameRVA, 1) || !in_image(base, imp->ImportAddressTableRVA, sizeof(IMAGE_THUNK_DATA64)))
                continue;
            const char* dll = reinterpret_cast<const char*>(base + imp->DllNameRVA);
            if (!dll_matches(dll, hook.dll)) continue;
            auto* names = imp->ImportNameTableRVA ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->ImportNameTableRVA) : nullptr;
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->ImportAddressTableRVA);
            patched = patch_slots(module, dll, names, slots, hook, true) || patched;
        }
    }
    return patched;
}

// Logs every message-loop import with where its slot points (by name, or by user32's real address).
// Returns true if the import table is a protector stub: imports present but none has a name table.
// Then every imported DLL is logged too (slot counts), for diagnosis.
bool scan_imports(HMODULE module) {
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool any_names = false;
    std::string summary;
    const char* pumps[] = {"PeekMessageW", "PeekMessageA", "GetMessageW", "GetMessageA", "MsgWaitForMultipleObjects",
                           "MsgWaitForMultipleObjectsEx", "DispatchMessageW", "DispatchMessageA"};
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    void* real[std::size(pumps)] = {};
    for (size_t i = 0; i < std::size(pumps); ++i)
        real[i] = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, pumps[i])) : nullptr;
    for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         in_image(base, reinterpret_cast<uintptr_t>(imp) - base, sizeof *imp) && imp->Name; ++imp) {
        if (!in_image(base, imp->Name, 1) || !in_image(base, imp->FirstThunk, sizeof(IMAGE_THUNK_DATA64))) continue;
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        auto* names = imp->OriginalFirstThunk ? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk) : nullptr;
        auto* slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        unsigned named = 0, ordinals = 0, total = 0;
        any_names = any_names || names;
        for (; in_image(base, reinterpret_cast<uintptr_t>(slots) - base, sizeof *slots) && slots->u1.Function;
             ++slots, names = names ? names + 1 : nullptr) {
            ++total;
            if (names && !in_image(base, reinterpret_cast<uintptr_t>(names) - base, sizeof *names)) names = nullptr;
            const void* to = reinterpret_cast<const void*>(slots->u1.Function);
            const char* name = nullptr;
            if (names && names->u1.AddressOfData) {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) ++ordinals;
                else if (in_image(base, names->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
                    ++named;
                    name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
                }
            }
            for (size_t i = 0; i < std::size(pumps); ++i) {
                const bool by_name = name && std::strcmp(name, pumps[i]) == 0;
                const bool by_address = real[i] && to == real[i];
                if (by_name || by_address)
                    log("imports:   %s!%s -> %s%s", dll, name ? name : "(no name)", describe(to).c_str(),
                        by_address ? "" : " (not user32's own address)");
            }
        }
        char line[160];
        std::snprintf(line, sizeof line, "\n  %s: %u slots (%u by name, %u by ordinal)%s", dll, total, named, ordinals,
                      names ? "" : ", no name table");
        summary += line;
    }
    if (any_names) return false;
    log("imports: exe import table (rva 0x%lx) has no name tables: protected exe, real imports resolved later%s",
        dir.VirtualAddress, summary.c_str());
    return true;
}

// Inline hook on the export itself, for protected exes. *original receives the trampoline.
bool inline_hook(Hook& hook) {
    HMODULE m = GetModuleHandleA(hook.dll);
    void* target = m ? reinterpret_cast<void*>(GetProcAddress(m, hook.function)) : nullptr;
    if (!target) {
        log("hooks: inline %s!%s: export not found (module %s)", hook.dll, hook.function, m ? "loaded" : "not loaded");
        return false;
    }
    static const MH_STATUS init = MH_Initialize();
    MH_STATUS status = init;
    if (status == MH_OK) status = MH_CreateHook(target, hook.replacement, reinterpret_cast<LPVOID*>(hook.original));
    if (status == MH_OK) status = MH_EnableHook(target);
    log("hooks: inline %s!%s at %s: %s", hook.dll, hook.function, describe(target).c_str(), MH_StatusToString(status));
    return status == MH_OK;
}

// Game modules (exe first); with pad_libraries, also the loaded libScePad*.dll modules (listed apart).
std::vector<HMODULE> loaded_game_modules(std::vector<HMODULE>* pad_libraries = nullptr) {
    std::vector<HMODULE> out;
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 2048);
    out.push_back(GetModuleHandleW(nullptr));
    for (size_t i = 0; i < n; ++i) {
        if (mods[i] == out.front()) continue;
        if (is_game_module(mods[i])) out.push_back(mods[i]);
        else if (pad_libraries && is_pad_library(mods[i])) pad_libraries->push_back(mods[i]);
    }
    return out;
}

// Patch hook i in module m once. Returns true if a slot points to the hook.
bool patch_once(HMODULE m, size_t i) {
    if (g_done.count({m, i})) return true;
    if (!patch_module(m, g_hooks[i])) return false;
    g_done.insert({m, i});
    return true;
}

void patch_all() {
    std::lock_guard lock(g_mutex);
    std::vector<HMODULE> pads;
    for (HMODULE m : loaded_game_modules(&pads))
        for (size_t i = 0; i < g_hooks.size(); ++i) patch_once(m, i);
    for (HMODULE m : pads)
        for (size_t i = 0; i < g_hooks.size(); ++i)
            if (g_hooks[i].pad_library && !g_done.count({m, i}) && patch_once(m, i))
                log("hooks: %s patched in the pad library module %p", g_hooks[i].function, static_cast<void*>(m));
}

HMODULE WINAPI hk_load_w(LPCWSTR name) {
    HMODULE m = g_load_w(name);
    if (m) patch_all();
    return m;
}
HMODULE WINAPI hk_load_a(LPCSTR name) {
    HMODULE m = g_load_a(name);
    if (m) patch_all();
    return m;
}
HMODULE WINAPI hk_load_ex_w(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE m = g_load_ex_w(name, file, flags);
    if (m && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        patch_all();
    return m;
}
HMODULE WINAPI hk_load_ex_a(LPCSTR name, HANDLE file, DWORD flags) {
    HMODULE m = g_load_ex_a(name, file, flags);
    if (m && !(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE)))
        patch_all();
    return m;
}

// Sony's pad library loaded: patched from a new thread, which starts once the loader lock is released (the DLL's
// imports are bound by then). Catches the load when an overlay owns the LoadLibrary imports (Dead Island 2: Epic's
// EOSOVH-Win64-Shipping.dll rewrites them, so hk_load_* never see the game's delay-load of libScePad.dll).
void on_pad_library_load(HMODULE) {
    if (HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { patch_all(); return 0; }, nullptr, 0, nullptr))
        CloseHandle(thread);
}

}  // namespace

void init(const std::wstring& module_prefix, bool force_inline) {
    g_prefix = lower(module_prefix);
    // EA's Origin/EA app activation wrapper (.ooa section: Mass Effect LE, Battlefield 4, Titanfall 2) keeps a small named
    // import table of its own, so the scan below can't tell; it loads the real imports (the proxy among them) itself.
    HMODULE exe = GetModuleHandleW(nullptr);
    const bool ooa = has_section(exe, ".ooa");
    if (ooa) log("imports: exe has an .ooa section (EA activation wrapper): protected exe, real imports resolved by it");
    g_inline = ooa || scan_imports(exe) || force_inline;
    // GYRO4ALL_INLINE=1 forces inline hooks (testing, or a protector this check doesn't recognize).
    wchar_t force[4] = {};
    if (GetEnvironmentVariableW(L"GYRO4ALL_INLINE", force, 4) && force[0] == L'1') g_inline = true;
    add(nullptr, "LoadLibraryW", reinterpret_cast<void*>(&hk_load_w), reinterpret_cast<void**>(&g_load_w));
    add(nullptr, "LoadLibraryA", reinterpret_cast<void*>(&hk_load_a), reinterpret_cast<void**>(&g_load_a));
    add(nullptr, "LoadLibraryExW", reinterpret_cast<void*>(&hk_load_ex_w), reinterpret_cast<void**>(&g_load_ex_w));
    add(nullptr, "LoadLibraryExA", reinterpret_cast<void*>(&hk_load_ex_a), reinterpret_cast<void**>(&g_load_ex_a));
    if (!g_inline) watch_dll(L"libScePad.dll", &on_pad_library_load);
    log("hooks: game modules prefix '%s', %zu loaded, %s", narrow(g_prefix).c_str(), loaded_game_modules().size(),
        g_inline ? "inline hooks on exports" : "import-table hooks");
}

namespace {

bool register_hook(const char* dll, WORD ordinal, const char* function, void* replacement, void** original, bool pad_library) {
    if (ordinal && !dll) return false;
    std::lock_guard lock(g_mutex);
    g_hooks.push_back({dll, function, replacement, original});
    const size_t index = g_hooks.size() - 1;
    g_hooks[index].ordinal = ordinal;
    g_hooks[index].pad_library = pad_library;
    // Protected exe: its import slots are filled by the protector later, so hook the export itself.
    // LoadLibrary hooks (dll null) only serve modular builds' later modules and are skipped there.
    if (g_inline) return g_hooks[index].dll && inline_hook(g_hooks[index]);
    bool any = false;
    std::vector<HMODULE> pads;
    for (HMODULE m : loaded_game_modules(&pads)) any = patch_once(m, index) || any;
    if (pad_library)
        for (HMODULE m : pads)
            if (patch_once(m, index)) log("hooks: %s patched in the pad library module %p", function, static_cast<void*>(m));
    return any;
}

}  // namespace

bool add(const char* dll, const char* function, void* replacement, void** original) {
    return register_hook(dll, 0, function, replacement, original, false);
}

bool add_ordinal(const char* dll, WORD ordinal, const char* function, void* replacement, void** original) {
    return register_hook(dll, ordinal, function, replacement, original, false);
}

bool add_with_pad_library(const char* dll, const char* function, void* replacement, void** original) {
    return register_hook(dll, 0, function, replacement, original, true);
}

bool inline_mode() { return g_inline; }

// Only the XInput imports are taken back: without them the translated controller never reaches the game (Mortal
// Shell: the Epic Online Services overlay, EOSOVH-Win64-Shipping.dll, rewrites them after Gyro4All patched them). Other
// imports (LoadLibrary*) are left to whoever rewrote them, so an overlay keeps its own hooks; that is logged once.
int verify() {
    std::lock_guard lock(g_mutex);
    int fixed = 0;
    for (Patched& p : g_patched) {
        const ULONGLONG now = *p.slot;
        if (now == reinterpret_cast<ULONGLONG>(p.replacement)) continue;
        const bool reclaim = std::strncmp(p.function, "XInput", 6) == 0;
        if (!p.reported)
            log("hooks: the import of %s!%s was written back by something else (now %s); %s", p.dll.c_str(), p.function,
                describe(reinterpret_cast<void*>(now)).c_str(), reclaim ? "patched again" : "left to it");
        p.reported = true;
        if (!reclaim) continue;
        DWORD old = 0;
        if (!VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
        *p.slot = reinterpret_cast<ULONGLONG>(p.replacement);
        VirtualProtect(p.slot, sizeof(void*), old, &old);
        ++fixed;
    }
    return fixed;
}

int redirect_slots(const char* dll, std::span<const Redirect> redirects) {
    if (redirects.empty()) return 0;
    const std::vector<HMODULE> modules = loaded_game_modules();
    std::lock_guard lock(g_mutex);
    int patched = 0;
    for (HMODULE module : modules) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
            if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            const uintptr_t begin = base + section->VirtualAddress, end = begin + section->Misc.VirtualSize;
            // Region by region: a protector may leave parts of its sections unreadable.
            for (uintptr_t at = begin; at < end;) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info)) break;
                const uintptr_t stop = std::min<uintptr_t>(end, reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize);
                const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
                if (info.State == MEM_COMMIT && (info.Protect & readable) && !(info.Protect & PAGE_GUARD)) {
                    for (auto* slot = reinterpret_cast<ULONGLONG*>((at + 7) & ~uintptr_t(7));
                         reinterpret_cast<uintptr_t>(slot + 1) <= stop; ++slot) {
                        const ULONGLONG value = *slot;
                        for (const Redirect& r : redirects) {
                            if (!r.target || value != reinterpret_cast<ULONGLONG>(r.target)) continue;
                            DWORD old = 0;
                            if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) break;
                            *slot = reinterpret_cast<ULONGLONG>(r.replacement);
                            VirtualProtect(slot, sizeof *slot, old, &old);
                            g_patched.push_back({slot, r.replacement, r.function, dll});
                            log("hooks: %s!%s slot at %s (an import table of the game's own) now points to Gyro4All", dll,
                                r.function, describe(slot).c_str());
                            ++patched;
                            break;
                        }
                    }
                }
                at = stop;
            }
        }
    }
    return patched;
}

bool hook_function(void* target, void* replacement, void** original) {
    static const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    return target && MH_CreateHook(target, replacement, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

namespace {

// ntdll's loader notifications (winternl.h has no declarations for them).
struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* full_name;
    const UNICODE_STRING* base_name;
    void* base;
    ULONG size;
};
using DllNotification_t = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using LdrRegisterDllNotification_t = LONG(NTAPI*)(ULONG flags, DllNotification_t callback, void* context, void** cookie);

struct Watch {
    std::wstring name;
    void (*on_load)(HMODULE);
};
std::mutex g_watch_mutex;
std::vector<Watch> g_watches;

VOID CALLBACK on_dll_notification(ULONG reason, const DllNotificationData* data, void*) {
    constexpr ULONG kLoaded = 1;
    if (reason != kLoaded || !data || !data->base_name || !data->base_name->Buffer) return;
    const std::wstring name(data->base_name->Buffer, data->base_name->Length / sizeof(wchar_t));
    std::vector<void (*)(HMODULE)> calls;
    {
        std::lock_guard lock(g_watch_mutex);
        for (const Watch& w : g_watches)
            if (_wcsicmp(name.c_str(), w.name.c_str()) == 0) calls.push_back(w.on_load);
    }
    for (auto call : calls) call(static_cast<HMODULE>(data->base));
}

}  // namespace

bool watch_dll(const wchar_t* name, void (*on_load)(HMODULE)) {
    static const bool registered = [] {
        auto reg = reinterpret_cast<LdrRegisterDllNotification_t>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
        void* cookie = nullptr;
        return reg && reg(0, &on_dll_notification, nullptr, &cookie) == 0;
    }();
    if (registered) {
        std::lock_guard lock(g_watch_mutex);
        g_watches.push_back({name, on_load});
    }
    if (HMODULE loaded = GetModuleHandleW(name)) {
        on_load(loaded);
        return true;
    }
    return registered;
}

std::string source_dll(const char* function) {
    std::lock_guard lock(g_mutex);
    for (const Hook& hook : g_hooks)
        if (!hook.from.empty() && std::strcmp(hook.function, function) == 0) return hook.from;
    return {};
}

std::vector<ImportedDll> imported_dlls() {
    std::lock_guard lock(g_mutex);
    std::vector<ImportedDll> out;
    for (HMODULE module : loaded_game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress)
            for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
                out.push_back({reinterpret_cast<const char*>(base + imp->Name), false});
        const auto& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay.VirtualAddress)
            for (auto* imp = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delay.VirtualAddress); imp->DllNameRVA; ++imp)
                if (imp->Attributes.RvaBased) out.push_back({reinterpret_cast<const char*>(base + imp->DllNameRVA), true});
    }
    return out;
}

std::vector<HMODULE> game_modules() {
    std::lock_guard lock(g_mutex);
    return loaded_game_modules();
}

bool modular() { return !g_prefix.empty(); }

}  // namespace gyro4all::hooks
