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
bool g_in_frame = false;
LARGE_INTEGER g_freq{}, g_last_frame{};

// One gyro frame, on the game thread: from the PeekMessageW hook, or from the engine module (Engine::frame_source).
void run_frame() {
    if (g_in_frame) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // At most one gyro frame per 0.5 ms, in case the game pumps several times per frame.
    if (now.QuadPart - g_last_frame.QuadPart < g_freq.QuadPart / 2000) return;
    g_last_frame = now;
    g_in_frame = true;
    g_frames_started = true;
    const int64_t start = gyro4all::perf::now();
    const bool active = gyro4all::host_frame();
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
// Vulkan) hands the session to it: the mutex is released before the child starts, so the child runs Gyro4All. Only
// before this process's first gyro frame: a game that already runs Gyro4All keeps it when it starts a helper exe.
HANDLE g_session = nullptr, g_legacy_session = nullptr;
std::wstring g_install_dir;  // lower case
using CreateProcessA_t = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
using CreateProcessW_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessA_t g_real_create_a = nullptr;
CreateProcessW_t g_real_create_w = nullptr;

// The child's exe: the application name, else the command line's first (possibly quoted) token, as a full path.
void hand_session_to(std::wstring app, const std::wstring& cmd) {
    if (!g_session || g_frames_started) return;
    if (app.empty()) {
        if (!cmd.empty() && cmd[0] == L'"') app = cmd.substr(1, cmd.find(L'"', 1) - 1);
        else app = cmd.substr(0, cmd.find(L' '));
    }
    wchar_t full[MAX_PATH];
    if (app.empty() || !GetFullPathNameW(app.c_str(), MAX_PATH, full, nullptr)) return;
    std::wstring path = lower(full);
    if (path.substr(0, path.find_last_of(L"\\/")) != g_install_dir) return;
    gyro4all::log("session: handed to %s (started by this process)", gyro4all::narrow(full).c_str());
    gyro4all::log_close();  // the child opens Gyro4All.log anew
    CloseHandle(g_session);
    if (g_legacy_session) CloseHandle(g_legacy_session);
    g_session = g_legacy_session = nullptr;
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

    // Only one process per install runs Gyro4All: Windows loads the proxy into every exe started from this
    // folder (the game, but also a config tool or benchmark shipped next to it), and two instances would
    // fight over the pad and Gyro4All.log. The first process to start owns a session mutex named after the
    // install folder (hashed: mutex names can't hold '\' and are limited to MAX_PATH); later ones stay a
    // plain system DLL proxy. The handle stays open until the process exits, which releases the mutex.
    const std::wstring mod_dir = dir + L"\\Gyro4All";
    const std::wstring folder = std::to_wstring(std::hash<std::wstring>{}(lower(dir)));
    HANDLE instance = CreateMutexW(nullptr, FALSE, (L"Local\\Gyro4All-" + folder).c_str());
    if (!instance) return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instance);
        return;
    }
    // Also the mutex of GyroUE (this project's former name, same hash): a GyroUE proxy left in this folder under the
    // other proxy name (winmm.dll / version.dll) loads into the same process. Whichever starts first runs; the other
    // stays a plain proxy.
    HANDLE legacy = CreateMutexW(nullptr, FALSE, (L"Local\\GyroUE-" + folder).c_str());
    if (legacy && GetLastError() == ERROR_ALREADY_EXISTS) {
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

    QueryPerformanceFrequency(&g_freq);
    g_game_thread = GetCurrentThreadId();
    gyro4all::settings::configure(data_dir, stem, mod_dir);
    gyro4all::settings::migrate_from_gyroue(dir);
    gyro4all::Engine& engine = gyro4all::select_engine(exe);
    if (engine.is_launcher(exe)) {
        gyro4all::log("session: %s is the game's launcher: no Gyro4All here, the session is left to the game it starts",
                      gyro4all::narrow(exe).c_str());
        gyro4all::log_close();  // the game opens Gyro4All.log anew
        if (legacy) CloseHandle(legacy);
        CloseHandle(instance);
        return;
    }
    gyro4all::settings::use_profile_alias(engine.profile_aliases(stem));
    g_adopt = engine.adopt_pump_thread();
    g_pump_class = engine.pump_window_class();
    if (g_pump_class) g_game_thread = 0;  // adopt_game_thread: the thread that pumps the game window
    gyro4all::hooks::init(engine.module_prefix(stem), engine.inline_hooks());
    gyro4all::host_configure(engine);
    gyro4all::overlay::install(data_dir + L"\\gyrolib.ini");
    if (engine.frame_source(&run_frame)) {
        gyro4all::log("hook: the %s module runs the gyro frame on its game thread", engine.name());
        return;
    }
    // Modular builds import PeekMessageW from one of the game's own DLLs (Unreal: ApplicationCore/Core) instead of the
    // exe; the hook covers every game module, including ones loaded later.
    if (!gyro4all::hooks::add("USER32.dll", "PeekMessageW", reinterpret_cast<void*>(&hooked_peek),
                              reinterpret_cast<void**>(&g_real_peek)))
        gyro4all::log("hook: PeekMessageW not imported by any loaded game module yet");
    g_session = instance;
    g_legacy_session = legacy;
    g_install_dir = lower(dir);
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
