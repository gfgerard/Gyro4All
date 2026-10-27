#pragma once
// Import-table hooks applied to every module of the game: the exe plus the game's own DLLs of modular builds
// ("<prefix>*.dll", the prefix comes from the engine module, e.g. Unreal's "<Project>-"). Modules loaded later (FModuleManager -> LoadLibrary) are patched as
// they load, so hooks registered in DllMain also reach renderer modules that load after startup.
#include <windows.h>

#include <span>
#include <string>
#include <vector>

namespace gyro4all::hooks {

// Call once from DllMain. module_prefix: lower-case file-name prefix of the game's own DLLs ("" = the exe only), from
// Engine::module_prefix (Unreal: "returnal-" for Returnal-Win64-Shipping.exe). force_inline: Engine::inline_hooks.
void init(const std::wstring& module_prefix, bool force_inline = false);

// Register an import to redirect in all game modules (regular and delay-load import tables). dll may be
// null to match any DLL name. *original receives the first resolved original function; it stays null
// when the only imports are delay-loads of a DLL that isn't loaded yet (resolve it from source_dll()).
// Patches already-loaded modules now. Returns true if at least one module was patched so far.
bool add(const char* dll, const char* function, void* replacement, void** original);

// Same for imports by ordinal from exactly `dll` (e.g. the Windows SDK's xinput.lib imports XINPUT1_4.dll!2).
// `function` is the export's name, used for logs, source_dll() and resolving the original.
bool add_ordinal(const char* dll, WORD ordinal, const char* function, void* replacement, void** original);

// Same as add(), and also in Sony's pad library when the game ships it as a separate DLL (libScePad*.dll, patched as
// it loads), for hooks that must see how that library opens the controller.
bool add_with_pad_library(const char* dll, const char* function, void* replacement, void** original);

// Import slots this module patched that were written back by something else (another mod, an overlay, the game) are
// patched again; the first time for each slot is logged. Returns how many were fixed. Any thread.
int verify();

// Inline hook (MinHook) on any function, e.g. a system DLL's export, for diagnostics that must see every caller.
// *original receives the trampoline. Returns false if the hook couldn't be created or enabled.
bool hook_function(void* target, void* replacement, void** original);

// Protected exes: the protector fills an import table of its own after DllMain, which the PE headers don't describe.
// Every pointer-aligned slot in the exe's non-executable sections that holds a redirect's target (dll!function's
// export) is pointed at its replacement and kept like an import slot (verify()); one sweep for all of them. Game thread
// or the watcher, once the game runs. Returns how many slots were patched now (0 if none hold a target yet).
struct Redirect {
    const char* function;
    void* target;
    void* replacement;
};
int redirect_slots(const char* dll, std::span<const Redirect> redirects);

// Calls on_load(module) once the named DLL (file name, e.g. L"sl.interposer.dll") loads, from ntdll's loader notification
// (inside the loader lock), and right away if it is already loaded; on_load must accept being called more than once.
// Returns false if loads can't be watched and the DLL isn't loaded.
bool watch_dll(const wchar_t* name, void (*on_load)(HMODULE));

// True when the exe's import table is a protector stub (or GYRO4ALL_INLINE=1): add() then hooks the
// named DLL's export inline instead of import slots.
bool inline_mode();

// Name of the DLL whose import of `function` was patched first (e.g. "XINPUT1_4.dll"), or "" if none yet.
std::string source_dll(const char* function);

// DLLs imported by the loaded game modules (regular and delay-load tables), as written in the import table.
struct ImportedDll {
    std::string name;
    bool delay = false;
};
std::vector<ImportedDll> imported_dlls();

// Game modules (exe first). Used for memory scans.
std::vector<HMODULE> game_modules();

// The game has DLLs of its own (a module prefix from the engine): some may load later, with imports not seen yet.
bool modular();

}  // namespace gyro4all::hooks
