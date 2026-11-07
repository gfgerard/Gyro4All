// Gyro4All entry point. Loaded as winmm.dll (or version.dll, see proxy.cpp) by the game exe (static import, so DllMain runs on the
// main thread, which in Unreal is the game thread). It picks the engine module for the game (engine.hpp) and patches the
// game modules' imports of PeekMessageW: the engine pumps Windows messages on the game thread once per frame, and the
// call that finds the queue empty marks the end of that pump, which is where the gyro frame runs.
#include "engine.hpp"
#include "hooks.hpp"
#include "host.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "perf.hpp"
#include "overlay.hpp"
#include "settings.hpp"

#include <windows.h>

#include <atomic>
#include <cwctype>
#include <format>
#include <intrin.h>
#include <string>

namespace {

using PeekMessageW_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

PeekMessageW_t g_real_peek = nullptr;
std::atomic<DWORD> g_game_thread{0};
std::atomic<bool> g_game_thread_pumped{false}, g_frames_started{false};
// g_turned: a gyro frame applied a camera turn (this process is the game). g_handed_over: the session went to an exe this
// process started (hand_session_to); no more gyro frames here.
std::atomic<bool> g_turned{false}, g_handed_over{false};
bool g_in_frame = false;
LARGE_INTEGER g_freq{}, g_last_frame{};

bool session_taken_over();
void session_turned();

// One gyro frame, on the game thread: from the PeekMessageW hook, or from the engine module (Engine::frame_source).
void run_frame() {
    if (g_in_frame || g_handed_over || session_taken_over()) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // At most one gyro frame per 0.5 ms, in case the game pumps several times per frame.
    if (now.QuadPart - g_last_frame.QuadPart < g_freq.QuadPart / 2000) return;
    g_last_frame = now;
    g_in_frame = true;
    g_frames_started = true;
    const int64_t start = gyro4all::perf::now();
    const bool active = gyro4all::host_frame();
    if (active && !g_turned.exchange(true)) session_turned();
    gyro4all::perf::record(active ? gyro4all::perf::Active : gyro4all::perf::Idle, start);
    g_in_frame = false;
}

// Diagnostics: every thread that pumps messages, once, with its caller, and every 10 s how often each pumped (a game
// whose game thread stops pumping after startup shows here which thread took over: Dead Space, MODLOG 2026-10-18).
struct Pumper {
    std::atomic<DWORD> thread{0};
    std::atomic<uint32_t> calls{0}, empty{0};
};
Pumper g_pumpers[8];
std::atomic<ULONGLONG> g_next_pump_report{0};

void note_pump(BOOL got, const void* caller) {
    const DWORD self = GetCurrentThreadId();
    Pumper* slot = nullptr;
    for (Pumper& p : g_pumpers) {
        DWORD t = p.thread.load(std::memory_order_relaxed);
        if (t == self) {
            slot = &p;
            break;
        }
        if (t == 0 && p.thread.compare_exchange_strong(t, self)) {
            slot = &p;
            gyro4all::log("hook: PeekMessageW on thread %lu%s, called from %s", self,
                          self == g_game_thread.load() ? " (the game thread)" : "",
                          gyro4all::mem::module_offset(reinterpret_cast<uintptr_t>(caller)).c_str());
            break;
        }
    }
    if (slot) {
        slot->calls.fetch_add(1, std::memory_order_relaxed);
        if (!got) slot->empty.fetch_add(1, std::memory_order_relaxed);
    }
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = g_next_pump_report.load(std::memory_order_relaxed);
    if (now < next || !g_next_pump_report.compare_exchange_strong(next, now + 10000)) return;
    std::string line;
    for (Pumper& p : g_pumpers)
        if (const DWORD t = p.thread.load())
            line += std::format(" {}:{}/{}", t, p.empty.exchange(0), p.calls.exchange(0));
    gyro4all::log("hook: PeekMessageW calls in the last 10 s (thread:empty/all):%s", line.c_str());
}

// Games whose DllMain thread never pumps (DOOM 2016 pumps on two other threads): before the first gyro frame, the
// thread that owns the game's visible top-level window becomes the game thread, and the only GyroLib owner from then on.
// Only for engines that ask for it (Engine::adopt_pump_thread): elsewhere a splash screen's thread could pump first.
bool g_adopt = false;
// Engines whose game window is pumped by a thread of its own (Engine::pump_window_class): the game thread is 0 until the
// first message for a top-level window of that class, then the thread that got it.
const wchar_t* g_pump_class = nullptr;
void adopt_game_thread(BOOL got, const MSG* msg) {
    const DWORD self = GetCurrentThreadId();
    if (g_pump_class) {
        if (g_game_thread || !got || !msg || !msg->hwnd) return;
        const HWND root = GetAncestor(msg->hwnd, GA_ROOT);
        wchar_t cls[64] = L"";
        if (!root || !GetClassNameW(root, cls, 64) || lstrcmpW(cls, g_pump_class) != 0) return;
        gyro4all::log("hook: thread %lu pumps the game window (class %s) and becomes the game thread", self,
                      gyro4all::narrow(cls).c_str());
        g_game_thread = self;
        return;
    }
    if (!g_adopt) return;
    if (self == g_game_thread) {
        g_game_thread_pumped = true;
        return;
    }
    if (g_game_thread_pumped || g_frames_started || !got || !msg || !msg->hwnd) return;
    if (GetAncestor(msg->hwnd, GA_ROOT) != msg->hwnd || !IsWindowVisible(msg->hwnd)) return;
    gyro4all::log("hook: game thread %lu never pumps; thread %lu owns the game window and becomes the game thread",
                  g_game_thread.load(), self);
    g_game_thread = self;
}

BOOL WINAPI hooked_peek(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    const BOOL got = g_real_peek(msg, hwnd, min, max, remove);
    note_pump(got, _ReturnAddress());
    adopt_game_thread(got, msg);
    if (got && msg && msg->message == WM_INPUT && GetCurrentThreadId() == g_game_thread)
        gyro4all::host_raw_input(reinterpret_cast<void*>(msg->lParam));
    if (!got && GetCurrentThreadId() == g_game_thread) run_frame();
    return got;
}

// ANSI builds pump with PeekMessageA instead (DOOM 2016, id Tech 6): the same end-of-pump frame.
PeekMessageW_t g_real_peek_a = nullptr;
BOOL WINAPI hooked_peek_a(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    const BOOL got = g_real_peek_a(msg, hwnd, min, max, remove);
    note_pump(got, _ReturnAddress());
    adopt_game_thread(got, msg);
    if (got && msg && msg->message == WM_INPUT && GetCurrentThreadId() == g_game_thread)
        gyro4all::host_raw_input(reinterpret_cast<void*>(msg->lParam));
    if (!got && GetCurrentThreadId() == g_game_thread) run_frame();
    return got;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

// Launchers: an exe that starts another exe of this folder (DOOM 2016's DOOMx64.exe relaunches DOOMx64vk.exe for
// Vulkan, RDR2's PlayRDR2.exe starts RDR2.exe) hands the session to it: the mutex is released before the child starts, so
// the child runs Gyro4All, and this process runs no more gyro frames (GyroLib stays loaded but is no longer polled). Only
// while this process has never turned a camera: a game that already runs Gyro4All keeps it when it starts a helper exe.
// Gyro frames alone don't count: engines with their own frame thread (RAGE) run them from the start, in the launcher too.
HANDLE g_session = nullptr, g_legacy_session = nullptr;
std::wstring g_install_dir;  // lower case
gyro4all::Engine* g_session_engine = nullptr;  // this process's engine, which tells its helper exes apart
using CreateProcessA_t = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
using CreateProcessW_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessA_t g_real_create_a = nullptr;
CreateProcessW_t g_real_create_w = nullptr;

// Launchers that start the game through another program (Steam's PlayRDR2.exe goes through the Rockstar Games Launcher,
// another folder, so hand_session_to never sees the game start): the session owner publishes in shared memory, until a
// deadline (kLauncherHandoverSeconds after it started), that a later exe of this folder may take the session over. A
// game exe of this folder (an engine detects it, not a launcher) that finds the session taken claims it while the
// window is open; the owner stops at its next gyro frame, closes its log and session handles and acknowledges. The
// window closes for good when the owner turns a camera (it is the game: a tool started next to it stays a plain proxy).
// Long enough for the Rockstar Games Launcher to sign in and update before it starts RDR2.exe.
constexpr int kLauncherHandoverSeconds = 180;
struct SharedSession {
    volatile LONG state;  // 0 = open, -1 = closed (camera turned or window off), > 0 = pid of the process that claimed it
    volatile LONG acked;  // the old owner stopped
    DWORD owner_pid;
    ULONGLONG deadline;   // GetTickCount64 (system-wide) at which the window closes
};
HANDLE g_shared_map = nullptr;
SharedSession* g_shared = nullptr;

void close_shared() {
    if (g_shared) UnmapViewOfFile(g_shared);
    if (g_shared_map) CloseHandle(g_shared_map);
    g_shared = nullptr;
    g_shared_map = nullptr;
}

// created: no process had it (an owner from an older Gyro4All, or none).
bool open_shared(const std::wstring& folder, bool& created) {
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedSession),
                                    (L"Local\\Gyro4All-session-" + folder).c_str());
    if (!map) return false;
    created = GetLastError() != ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSession));
    if (!view) {
        CloseHandle(map);
        return false;
    }
    g_shared_map = map;
    g_shared = static_cast<SharedSession*>(view);
    return true;
}

// DllMain of a later process that found the session taken: true when it claimed it (and should run Gyro4All).
bool take_session_over(const std::wstring& folder, const std::wstring& exe) {
    bool game = false;
    for (gyro4all::Engine* e : gyro4all::all_engines())
        if (e->detect(exe) && !e->is_launcher(exe)) game = true;
    bool created = false;
    if (!game || !open_shared(folder, created)) return false;
    if (created || GetTickCount64() >= g_shared->deadline ||
        InterlockedCompareExchange(&g_shared->state, LONG(GetCurrentProcessId()), 0) != 0) {
        close_shared();
        return false;
    }
    // The owner stops at its next gyro frame; a frozen or frameless owner isn't waited for long. Its log is closed
    // before this process opens (and truncates) it.
    for (int i = 0; i < 300 && !g_shared->acked; ++i) Sleep(10);
    return true;
}

// The owner, once its settings are read: opens the window (again, after a takeover: from this process's start).
void open_session_window(const std::wstring& folder, int seconds) {
    if (!g_shared) {
        bool created = false;
        if (!open_shared(folder, created)) return;
    }
    g_shared->owner_pid = GetCurrentProcessId();
    g_shared->deadline = GetTickCount64() + ULONGLONG(seconds > 0 ? seconds : 0) * 1000;
    g_shared->acked = 0;
    InterlockedExchange(&g_shared->state, seconds > 0 ? 0 : -1);
    if (seconds > 0)
        gyro4all::log("session: for the next %d s, a game exe of this folder started later (by a launcher) would take "
                      "Gyro4All over, unless a camera turns here first", seconds);
}

void session_turned() {
    if (g_shared) InterlockedCompareExchange(&g_shared->state, -1, 0);
}

bool session_taken_over() {
    if (!g_shared) return false;
    const LONG claimer = g_shared->state;
    if (claimer <= 0 || DWORD(claimer) == GetCurrentProcessId()) return false;
    g_handed_over = true;
    gyro4all::log("session: taken over by process %ld (a game exe of this folder, started later); the gyro stops here",
                  claimer);
    gyro4all::log_close();
    if (g_session) CloseHandle(g_session);
    if (g_legacy_session) CloseHandle(g_legacy_session);
    g_session = g_legacy_session = nullptr;
    InterlockedExchange(&g_shared->acked, 1);
    close_shared();
    return true;
}

// The child's exe: the application name, else the command line's first (possibly quoted) token, as a full path.
void hand_session_to(std::wstring app, const std::wstring& cmd) {
    if (!g_session || g_turned) return;
    if (app.empty()) {
        if (!cmd.empty() && cmd[0] == L'"') app = cmd.substr(1, cmd.find(L'"', 1) - 1);
        else app = cmd.substr(0, cmd.find(L' '));
    }
    wchar_t full[MAX_PATH];
    if (app.empty() || !GetFullPathNameW(app.c_str(), MAX_PATH, full, nullptr)) return;
    std::wstring path = lower(full);
    if (path.substr(0, path.find_last_of(L"\\/")) != g_install_dir) return;
    // Only a real exe of this folder takes the session: a bare argument (Unity passes its PID first, with no
    // application name) resolves into the game folder too. CreateProcess adds ".exe" to a name without extension.
    std::wstring exe_path = full;
    const size_t name_at = path.find_last_of(L"\\/") + 1;
    if (path.find(L'.', name_at) == std::wstring::npos) exe_path += L".exe";
    const DWORD attrs = GetFileAttributesW(exe_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) || exe_path.size() < 4 ||
        lower(exe_path.substr(exe_path.size() - 4)) != L".exe") {
        gyro4all::log("session: kept (the started process '%s' is not an exe of this folder)",
                      gyro4all::narrow(full).c_str());
        return;
    }
    // A helper the engine knows (Cyberpunk starts REDEngineErrorReporter.exe at launch): the game keeps the session.
    if (g_session_engine && g_session_engine->is_launcher(full)) {
        gyro4all::log("session: kept (%s, started by this process, is a helper)", gyro4all::narrow(full).c_str());
        return;
    }
    g_handed_over = true;
    gyro4all::log("session: handed to %s (started by this process%s)", gyro4all::narrow(full).c_str(),
                  g_frames_started ? "; the gyro stops here" : "");
    gyro4all::log_close();  // the child opens Gyro4All.log anew
    CloseHandle(g_session);
    if (g_legacy_session) CloseHandle(g_legacy_session);
    g_session = g_legacy_session = nullptr;
    close_shared();
}

std::wstring widen(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

BOOL WINAPI hooked_create_a(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                            DWORD flags, LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi) {
    hand_session_to(widen(app), widen(cmd));
    return g_real_create_a(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
}

BOOL WINAPI hooked_create_w(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                            DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    hand_session_to(app ? app : L"", cmd ? cmd : L"");
    return g_real_create_w(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
}

// True when a file can be created in dir.
bool writable(const std::wstring& dir) {
    HANDLE file = CreateFileW((dir + L"\\.gyro4all-write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

// For games whose folder users can't write to (the EA app makes its installs in Program Files read-only again on every
// launch): %LOCALAPPDATA%\Gyro4All\<exe stem>, with the shipped profiles copied in when they are missing or newer there.
// "" if it can't be created.
std::wstring user_data_dir(const std::wstring& install_dir, const std::wstring& stem) {
    wchar_t local[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    const std::wstring dir = std::wstring(local) + L"\\Gyro4All\\" + stem;
    CreateDirectoryW((std::wstring(local) + L"\\Gyro4All").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\profiles").c_str(), nullptr);
    if (!writable(dir)) return {};
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((install_dir + L"\\profiles\\*.ini").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring from = install_dir + L"\\profiles\\" + found.cFileName;
            const std::wstring to = dir + L"\\profiles\\" + found.cFileName;
            WIN32_FILE_ATTRIBUTE_DATA there{};
            if (!GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &there) ||
                CompareFileTime(&found.ftLastWriteTime, &there.ftLastWriteTime) > 0)
                CopyFileW(from.c_str(), to.c_str(), FALSE);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return dir;
}

void attach(HMODULE self) {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(self, buffer, MAX_PATH);
    std::wstring dir = buffer;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring exe = buffer;
    std::wstring stem = exe.substr(exe.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.find_last_of(L'.'));

    // The Xbox app's launcher (GameLaunchHelper.exe, next to every PC Game Pass game's exe) starts the game without
    // CreateProcess, so the session can't be handed over: it never runs Gyro4All (Wolfenstein: The New Order).
    if (lower(stem) == L"gamelaunchhelper") return;
    // A launcher or helper exe of a detected engine (Cyberpunk's REDEngineErrorReporter.exe, which the game starts at
    // launch and again when it crashes) stays a plain proxy before it touches the session or Gyro4All.log: opening the
    // log truncates it, and the game's log was lost that way (MODLOG 2026-11-04). The check after select_engine below
    // still covers an engine forced with Engine=.
    for (gyro4all::Engine* e : gyro4all::all_engines())
        if (e->detect(exe) && e->is_launcher(exe)) return;

    // Only one process per install runs Gyro4All: Windows loads the proxy into every exe started from this
    // folder (the game, but also a config tool or benchmark shipped next to it), and two instances would
    // fight over the pad and Gyro4All.log. The first process to start owns a session mutex named after the
    // install folder (hashed: mutex names can't hold '\' and are limited to MAX_PATH); later ones stay a
    // plain system DLL proxy. The handle stays open until the process exits, which releases the mutex.
    const std::wstring mod_dir = dir + L"\\Gyro4All";
    const std::wstring folder = std::to_wstring(std::hash<std::wstring>{}(lower(dir)));
    HANDLE instance = CreateMutexW(nullptr, FALSE, (L"Local\\Gyro4All-" + folder).c_str());
    if (!instance) return;
    bool took_over = false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        took_over = take_session_over(folder, exe);
        if (!took_over) {
            CloseHandle(instance);
            return;
        }
    }
    // Also the mutex of GyroUE (this project's former name, same hash): a GyroUE proxy left in this folder under the
    // other proxy name (winmm.dll / version.dll) loads into the same process. Whichever starts first runs; the other
    // stays a plain proxy.
    HANDLE legacy = CreateMutexW(nullptr, FALSE, (L"Local\\GyroUE-" + folder).c_str());
    if (legacy && GetLastError() == ERROR_ALREADY_EXISTS && !took_over) {
        CloseHandle(legacy);
        CloseHandle(instance);
        return;
    }

    CreateDirectoryW(mod_dir.c_str(), nullptr);
    std::wstring data_dir = mod_dir;
    if (!writable(mod_dir)) {
        const std::wstring fallback = user_data_dir(mod_dir, stem);
        if (!fallback.empty()) data_dir = fallback;
    }
    gyro4all::log_open(data_dir + L"\\Gyro4All.log");
    gyro4all::log("Gyro4All " GYRO4ALL_VERSION " in %s", gyro4all::narrow(exe).c_str());
    if (data_dir != mod_dir)
        gyro4all::log("settings: %s is read-only; settings, profiles and logs are in %s",
                      gyro4all::narrow(mod_dir).c_str(), gyro4all::narrow(data_dir).c_str());
    if (took_over) gyro4all::log("session: taken over from the process that started Gyro4All first (a launcher)");

    QueryPerformanceFrequency(&g_freq);
    g_game_thread = GetCurrentThreadId();
    gyro4all::settings::configure(data_dir, stem, mod_dir);
    gyro4all::settings::migrate_from_gyroue(dir);
    gyro4all::Engine& engine = gyro4all::select_engine(exe);
    g_session_engine = &engine;
    if (engine.is_launcher(exe)) {
        gyro4all::log("session: %s is the game's launcher: no Gyro4All here, the session is left to the game it starts",
                      gyro4all::narrow(exe).c_str());
        gyro4all::log_close();  // the game opens Gyro4All.log anew
        if (legacy) CloseHandle(legacy);
        CloseHandle(instance);
        close_shared();
        return;
    }
    gyro4all::settings::use_profile_alias(engine.profile_aliases(stem));
    g_adopt = engine.adopt_pump_thread();
    g_pump_class = engine.pump_window_class();
    if (g_pump_class) g_game_thread = 0;  // adopt_game_thread: the thread that pumps the game window
    gyro4all::hooks::init(engine.module_prefix(stem), engine.inline_hooks());
    gyro4all::host_configure(engine);
    gyro4all::overlay::install(data_dir + L"\\gyrolib.ini");
    g_session = instance;
    g_legacy_session = legacy;
    g_install_dir = lower(dir);
    open_session_window(folder, kLauncherHandoverSeconds);
    // Inline on the export, not in the game modules' import tables: the relaunch can come from any DLL (Kena's
    // CODEX64.dll restarts the exe through its own CreateProcessW import). kernel32's CreateProcess* only jump to
    // KERNELBASE's.
    HMODULE base = GetModuleHandleW(L"kernelbase.dll");
    if (!base) base = GetModuleHandleW(L"kernel32.dll");
    void* create_a = base ? reinterpret_cast<void*>(GetProcAddress(base, "CreateProcessA")) : nullptr;
    void* create_w = base ? reinterpret_cast<void*>(GetProcAddress(base, "CreateProcessW")) : nullptr;
    if (!create_a || !create_w ||
        !gyro4all::hooks::hook_function(create_a, reinterpret_cast<void*>(&hooked_create_a),
                                        reinterpret_cast<void**>(&g_real_create_a)) ||
        !gyro4all::hooks::hook_function(create_w, reinterpret_cast<void*>(&hooked_create_w),
                                        reinterpret_cast<void**>(&g_real_create_w)))
        gyro4all::log("session: CreateProcess hook failed: a relaunched game exe won't run Gyro4All");
    if (engine.frame_source(&run_frame)) {
        gyro4all::log("hook: the %s module runs the gyro frame on its game thread", engine.name());
        return;
    }
    // Modular builds import PeekMessageW from one of the game's own DLLs (Unreal: ApplicationCore/Core) instead of the
    // exe; the hook covers every game module, including ones loaded later.
    if (!gyro4all::hooks::add("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&hooked_peek),
                              reinterpret_cast<void**>(&g_real_peek)))
        gyro4all::log("hook: PeekMessageW not imported by any loaded game module yet");
    if (gyro4all::hooks::add("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&hooked_peek_a),
                             reinterpret_cast<void**>(&g_real_peek_a)))
        gyro4all::log("hook: PeekMessageA hook registered too");
    gyro4all::log("hook: PeekMessageW hook registered, game thread %lu", g_game_thread.load());
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        attach(instance);
    }
    return TRUE;
}
