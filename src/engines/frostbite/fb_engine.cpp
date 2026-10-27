// Frostbite (EA): Dead Space (2023) first. The game is detected, the gyro frame runs (inline PeekMessageW hook: the exe
// is protected by Denuvo and EA's OOA, which resolve its imports themselves), the engine's TypeInfo objects are found
// (fb_types.hpp) and written to Gyro4All-types.txt as a readable SDK, and the game's camera model (fb_camera.hpp)
// finds its classes and code from them and hooks the game's camera update, where the gyro's turn is applied.
#include "fb.hpp"
#include "fb_camera.hpp"
#include "fb_types.hpp"
#include "fb_xinput.hpp"

#include "dinputpad.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "overlay.hpp"
#include "settings.hpp"
#include "writewatch.hpp"

#include <windows.h>

#include <atomic>
#include <format>
#include <memory>

namespace gyro4all::fb {

namespace {

const char kSettings[] =
    "; Frostbite, for finding the camera of a new game: when no camera model fits the game, Gyro4All writes every type's\r\n"
    ";     TypeInfo to Gyro4All\\Gyro4All-types.txt on start. TypeDump= writes it in every game instead: comma-separated\r\n"
    ";     parts of type names (e.g. camera,Player), * = every type; 0 = never.\r\n"
    ";TypeDump=*\r\n";

std::string g_type_dump;  // TypeDump= ("" = only when no camera model fits)
std::atomic<int> g_stage{0};
std::atomic<size_t> g_type_count{0};
TypeTable g_types;  // written by the attach thread only, until g_stage says Ready
std::unique_ptr<Camera> g_camera;      // the game's camera model, written by the attach thread before g_camera_ready
std::atomic<bool> g_camera_ready{false};
std::string g_camera_why = "no camera model resolved yet";  // attach thread, then read once g_camera_tried is set
std::atomic<bool> g_camera_tried{false};

void write_types(const std::string& filter, const char* why) {
    const size_t n = dump_types(g_types, settings::mod_dir() + L"\\Gyro4All-types.txt", filter == "*" ? "" : filter);
    log("fb: TypeDump: %zu types written to Gyro4All-types.txt (%s)", n, why);
}

// Gyro4All's frame: Dead Space's game thread stops pumping window messages ~10 s after start (MODLOG 2026-10-18), so
// the PeekMessageW hook never runs the frame in play. Instead this module's own thread runs it, woken by each Present of
// the game's swapchain (overlay::on_present): one stable thread for GyroLib, once per rendered frame, whatever thread
// presents, and no work on the render thread. Without Present (the panel's graphics hooks are off, or loading) it runs
// on a timer. The camera itself is turned on the game's job threads, from the update hook (fb_camera.hpp).
Engine::FrameFn g_frame = nullptr;
HANDLE g_present_event = nullptr;
HANDLE g_frame_done = nullptr;  // set after each frame (sync_gyro_frame)
std::atomic<uint32_t> g_frames_started{0}, g_frames_done{0};
std::atomic<ULONGLONG> g_last_present{0};

void on_present() {
    g_last_present.store(GetTickCount64(), std::memory_order_relaxed);
    SetEvent(g_present_event);
}

DWORD WINAPI frame_thread(void*) {
    log("fb: gyro frame thread %lu, woken by the game's Present (8 ms timer without it)", GetCurrentThreadId());
    for (;;) {
        const bool presenting = GetTickCount64() - g_last_present.load(std::memory_order_relaxed) < 1000;
        WaitForSingleObject(g_present_event, presenting ? 50 : 8);
        ++g_frames_started;
        g_frame();
        ++g_frames_done;
        SetEvent(g_frame_done);
    }
}

}  // namespace

bool sync_gyro_frame(unsigned timeout_ms) {
    if (!g_present_event || !g_frame_done) return false;
    // A frame that starts after this call: one running now may have read the gyro before it.
    const uint32_t wanted = g_frames_started.load() + 1;
    SetEvent(g_present_event);
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (int32_t(g_frames_done.load() - wanted) < 0) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until || WaitForSingleObject(g_frame_done, DWORD(until - now)) == WAIT_TIMEOUT) return false;
    }
    return true;
}

namespace {

const char* const kStages[] = {"waiting for the types", "types found", "no types found"};
enum Stage { WaitTypes, Ready, Failed };

DWORD WINAPI attach_thread(void*) {
    // The TypeInfo objects are registered by the exe's static initialisers, after the protector has unpacked it.
    const ULONGLONG deadline = GetTickCount64() + 300000;
    std::string why, last_why;
    Sleep(5000);
    for (;;) {
        const ULONGLONG start = GetTickCount64();
        if (find_types(g_types, why)) break;
        if (why != last_why) log("fb: %s (%llu ms)", why.c_str(), GetTickCount64() - start);
        last_why = why;
        if (GetTickCount64() > deadline) {
            g_stage = Failed;
            log("fb: no Frostbite types found; the gyro can't run in this game");
            return 0;
        }
        Sleep(5000);
    }
    g_type_count = g_types.types.size();
    g_stage = Ready;
    log("fb: %zu types", g_types.types.size());
    if (!g_type_dump.empty() && g_type_dump != "0") write_types(g_type_dump, "TypeDump=");
    // The first camera model whose classes this game has.
    std::string reasons;
    for (auto make : {&make_deadspace_camera, &make_bf4_camera, &make_mea_camera}) {
        std::unique_ptr<Camera> camera = make();
        const ULONGLONG start = GetTickCount64();
        std::string reason;
        if (camera->resolve(g_types, reason)) {
            log("fb: camera model %s resolved (%llu ms)", camera->game(), GetTickCount64() - start);
            if (!camera->install(reason)) {
                log("fb: camera model %s: %s", camera->game(), reason.c_str());
                reasons += std::string(reasons.empty() ? "" : "; ") + camera->game() + ": " + reason;
                break;
            }
            g_camera = std::move(camera);
            g_camera_ready = true;
            writewatch::provide("cam", [] { return g_camera->watch_address(); });
            break;
        }
        log("fb: camera model %s: %s", camera->game(), reason.c_str());
        reasons += std::string(reasons.empty() ? "" : "; ") + camera->game() + ": " + reason;
    }
    if (!g_camera_ready) {
        g_camera_why = "no camera model fits (" + reasons + ")";
        if (g_type_dump.empty()) write_types("*", "no camera model fits: for finding this game's camera");
    }
    g_camera_tried = true;
    return 0;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

class FrostbiteEngine final : public Engine {
public:
    const char* name() const override { return "Frostbite"; }

    // The game's data: Data\layout.toc and Data\initfs_Win32 next to the exe.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\Data\\layout.toc") && exists(dir + L"\\Data\\initfs_Win32");
    }

    bool inline_hooks() const override { return true; }

    const char* default_settings() const override { return kSettings; }

    void configure() override {
        g_type_dump = settings::get_string(L"TypeDump", L"");
        log("settings: TypeDump=%s", g_type_dump.empty() ? "(unset: only when no camera model fits)" : g_type_dump.c_str());
        dinputpad::on_create(&hook_xinput_device_check);
        if (HANDLE thread = CreateThread(nullptr, 0, &attach_thread, nullptr, 0, nullptr)) CloseHandle(thread);
        else log("fb: can't start the attach thread (error %lu)", GetLastError());
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        g_present_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_frame_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_present_event) {
            log("fb: can't create the Present event (error %lu); the gyro frame stays on PeekMessageW", GetLastError());
            return false;
        }
        overlay::on_present(&on_present);
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("fb: can't start the gyro frame thread (error %lu); the gyro frame stays on PeekMessageW", GetLastError());
        overlay::on_present(nullptr);
        return false;
    }

    // A gameplay view while the camera model's camera runs and takes input (menus, pause, cutscenes: none).
    EngineFrame read_frame() override {
        EngineFrame f;
        if (!g_camera_ready) {
            f.target = "no camera";
            // No camera model fits this game (or its types weren't found): no hook for its camera, so the gyro goes to
            // the game as right-stick input (the core's stick fallback, blind). Before that's known: nothing.
            if (g_camera_tried || g_stage == Failed) {
                f.no_camera = true;
                f.in_game = true;
                f.view_id = 1;
                f.target = "no camera hook (gyro as right stick)";
            }
            return f;
        }
        const CameraFrame c = g_camera->read();
        f.in_game = c.in_game;
        f.view_id = c.active ? 1 : 0;
        f.target = c.target;
        return f;
    }

    bool apply(double pitch, double yaw) override { return g_camera_ready && g_camera->turn(pitch, yaw); }

    std::string status_scene() const override {
        const std::string camera = g_camera_ready ? g_camera->status()
                                   : g_camera_tried ? g_camera_why
                                                    : std::string("no camera yet");
        return std::format("fb: {} ({} types), {}", kStages[g_stage.load()], g_type_count.load(), camera);
    }
};

}  // namespace

Engine& engine() {
    static FrostbiteEngine instance;
    return instance;
}

}  // namespace gyro4all::fb
