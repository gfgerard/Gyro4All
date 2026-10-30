// Northlight (Remedy): Control, CONTROL Resonant and Alan Wake 2 (games/Control/MODDING_PLAN.md, MODLOG 2026-10-29).
// Control: the gyro turns the gameplay camera's look direction (control_camera.cpp) while no mouse cursor shows;
// without its class (another build) the gyro goes to the game as right-stick input (the core's blind stick fallback).
// Alan Wake 2: the gyro turns its camera state's look direction in a hook on the camera update (aw2_camera.cpp). CONTROL
// Resonant: detection only, the gyro as right stick (the Alan Wake 2 hook's code isn't there, presumably).
//
// Control is a modular build: the exe (Control_DX12.exe or Control_DX11.exe) is thin and the engine lives in
// <module>_rmdwin10_f.dll (DX12) or <module>_rmdwin7_f.dll (DX11) next to it; the game is detected by its coregame
// module. Control.exe in the same folder is the .NET launcher that starts one of the two. The message pump
// (PeekMessageW/GetMessageW) is imported by app_rmdwin*_f.dll, not the exe, on a thread of its own (the game window's).
// The pads are read through XInput loaded at run time by rl_rmdwin*_f.dll (LoadLibrary xinput1_4/1_3/9_1_0 and
// GetProcAddress XInputGetState), whose stored pointers are redirected (module_prefix, inline_hooks).
//
// CONTROL Resonant is one exe (CONTROLResonant.exe, DX12 only) that imports the pump, XInputGetState (xinput1_4
// ordinal 2) and HID itself (Sony's pad library is linked in statically; GameInput is loaded at run time). Its data is
// in data_pack2\<generic|pc>\*.rmdtoc/*.rmdblob, which detects it, and Alan Wake 2 (AlanWake2.exe, the same layout).
// Both pump their window (AppWindowClass) on a thread of its own with one PeekMessageW, then a GetMessageW loop
// (AlanWake2.exe+0x1859481, CONTROLResonant.exe+0x3fe0b0; MODLOG 2026-10-30): the gyro frame runs on this module's
// own thread instead (frame_thread).
#include "northlight.hpp"

#include "aw2_camera.hpp"
#include "control_camera.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "overlay.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <string>

namespace gyro4all::northlight {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring file_name(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

bool any_file(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

std::wstring folder(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

// Control: the coregame module of either renderer next to the exe.
bool is_control(const std::wstring& dir) {
    return exists(dir + L"\\coregame_rmdwin10_f.dll") || exists(dir + L"\\coregame_rmdwin7_f.dll");
}

// This process is Control (not CONTROL Resonant).
bool control_game() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return is_control(folder(exe));
}

// Games that pump with GetMessageW: Gyro4All's frame on a thread of its own, woken by each Present of the game's
// swapchain (overlay::on_present), on an 8 ms timer while nothing presents (as Frostbite's, fb_engine.cpp).
Engine::FrameFn g_frame = nullptr;
HANDLE g_present_event = nullptr;
std::atomic<ULONGLONG> g_last_present{0};

void on_present() {
    g_last_present.store(GetTickCount64(), std::memory_order_relaxed);
    SetEvent(g_present_event);
}

DWORD WINAPI frame_thread(void*) {
    log("northlight: gyro frame thread %lu, woken by the game's Present (8 ms timer without it)", GetCurrentThreadId());
    for (;;) {
        const bool presenting = GetTickCount64() - g_last_present.load(std::memory_order_relaxed) < 1000;
        WaitForSingleObject(g_present_event, presenting ? 50 : 8);
        g_frame();
    }
}

bool cursor_shown(CURSORINFO& ci) {
    ci = {sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

class NorthlightEngine final : public Engine {
public:
    const char* name() const override { return "Northlight"; }

    // Control by its modules; CONTROL Resonant by its packs' tables of contents.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = folder(exe_path);
        return is_control(dir) || any_file(dir + L"\\data_pack2\\pc\\*.rmdtoc");
    }

    // Control: rl_rmdwin10_f.dll / rl_rmdwin7_f.dll keep the XInput functions they get through GetProcAddress in their
    // data (rl+0x3a28a0 XInputGetState, +0x3a2898 XInputSetState in the 2025 DX12 build), which the core points at
    // Gyro4All as a game module's import slots. CONTROL Resonant: the exe imports everything itself.
    std::wstring module_prefix(const std::wstring& /*exe_stem*/) const override {
        return control_game() ? L"rl_rmdwin" : L"";
    }

    // Control: no import slot sees the game's XInput polls, and the Steam overlay hooks xinput1_4's exports in front of
    // Gyro4All's and answers itself (MODLOG 2026-10-29): inline hooks, whose watcher redirects rl's pointers. The pump
    // (app_rmdwin*_f.dll) is then caught by the inline PeekMessageW hook.
    bool inline_hooks() const override { return control_game(); }

    // Control's window (AppWindowClass) is made and pumped by a thread other than the DllMain one, which becomes the game
    // thread (MODLOG 2026-10-29: PeekMessageW from app_rmdwin10_f.dll+0x548F on that thread only).
    const wchar_t* window_class() const override { return L"AppWindowClass"; }
    const wchar_t* pump_window_class() const override { return control_game() ? L"AppWindowClass" : nullptr; }

    // The others pump with GetMessageW (see the top): the frame on frame_thread.
    bool frame_source(FrameFn frame) override {
        if (control_game()) return false;
        g_frame = frame;
        g_present_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_present_event) {
            log("northlight: can't create the Present event (error %lu); no gyro frame", GetLastError());
            return false;
        }
        overlay::on_present(&on_present);
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("northlight: can't start the gyro frame thread (error %lu); no gyro frame", GetLastError());
        overlay::on_present(nullptr);
        return false;
    }

    // Control.exe picks the renderer and starts Control_DX12.exe or Control_DX11.exe.
    bool is_launcher(const std::wstring& exe_path) const override {
        return is_control(folder(exe_path)) && _wcsicmp(file_name(exe_path).c_str(), L"Control.exe") == 0;
    }

    void configure() override {
        control_ = control_game();
        if (!control_) aw2_ = aw2::install();
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // Control: the default view while the camera movement component is there and no mouse cursor is shown; nothing while
    // it isn't (loading, the front end). Without its class, and in CONTROL Resonant: the gyro as right stick.
    EngineFrame read_frame() override {
        EngineFrame f;
        const bool was = cursor_;
        CURSORINFO ci;
        cursor_ = cursor_shown(ci);
        if (cursor_ != was)
            log("northlight: mouse cursor %s (flags %lu, cursor %p, at %ld,%ld)", cursor_ ? "shown (menu)" : "hidden",
                ci.flags, static_cast<void*>(ci.hCursor), ci.ptScreenPos.x, ci.ptScreenPos.y);
        f.cursor = cursor_;
        if (aw2_) {
            // A gameplay view while the camera update runs (none in menus, loading, cutscenes).
            double p = 0, h = 0;
            if (!aw2::look(p, h)) return f;
            widen_limits(p);
            f.in_game = true;
            f.view_id = 1;
            f.target = "camera state (Alan Wake 2)";
            return f;
        }
        search_ = control_ ? control::poll() : control::Search::NoVtable;
        if (search_ == control::Search::NoVtable) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        if (const uintptr_t c = control::component(); c && !control::valid(c)) control::lost();
        if (!control::component()) return f;
        if (double p = 0, h = 0; control::look(control::component(), p, h)) widen_limits(p);
        f.in_game = true;
        f.view_id = 1;
        f.target = "GamePlayerCameraMovementComponent";
        return f;
    }

    // Trims the pitch to the limits, so the gyro stops there instead of pushing past straight up or down.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        double p = 0, h = 0;
        if (aw2_) {
            if (!aw2::look(p, h)) return;
            p += aw2::pending_pitch();
        } else {
            if (!control::component() || !control::look(control::component(), p, h)) return;
            if (control::hooked()) p += control::pending_pitch();
        }
        pitch = std::clamp(p + pitch, -max_down_, max_up_) - p;
    }

    // Degrees, the gyro's senses (pitch up, yaw right), as the look direction's. With the update hooked, added for the
    // game's next update of the camera to take; else written now.
    bool apply(double pitch, double yaw) override {
        if (aw2_) {
            if (!aw2::live()) return false;
            aw2::add(pitch, yaw, max_up_, max_down_);
            return true;
        }
        const uintptr_t c = control::component();
        if (!c) return false;
        if (control::hooked()) {
            control::add(pitch, yaw, max_up_, max_down_);
            return true;
        }
        return control::turn(c, pitch, yaw, max_up_, max_down_);
    }

    // For the core's stick fallback only. With the update hooked, none: the hook's writes are the game's own update (they
    // always stay), and the fallback still switched on when the game itself turned the camera against the gyro for 8
    // frames (aim assist, presumably), the gyro then also going to the game as right stick (MODLOG 2026-10-30). Read back
    // right after apply() the camera hadn't moved yet either, until the queued turn was counted in.
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        camera = aw2_ ? aw2::live() : control::component();
        return camera && !aw2_ && !control::hooked() && control::look(camera, pitch, yaw);
    }

    // Only the mouse cursor tells menus apart: GyroLib's "menus only" automatic calibration would never run.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (aw2_) {
            double p = 0, h = 0;
            if (!aw2::look(p, h)) return cursor_ ? "view=menu (cursor)" : "view=none (camera update not running)";
            return std::format("view={} camera={} pitch={:.1f} (limits -{:.0f}..{:.0f}) heading={:.1f}",
                               cursor_ ? "menu (cursor)" : "default", reinterpret_cast<void*>(aw2::live()), p, max_down_,
                               max_up_, h);
        }
        const uintptr_t c = control_ ? control::component() : 0;
        if (!c) {
            if (search_ != control::Search::NoVtable) return "view=none (no camera movement component)";
            return cursor_ ? "view=menu (cursor)" : "view=default (gyro as right stick)";
        }
        double p = 0, h = 0;
        control::look(c, p, h);
        return std::format("view={} camera={} pitch={:.1f} (limits -{:.0f}..{:.0f}) heading={:.1f}",
                           cursor_ ? "menu (cursor)" : "default", reinterpret_cast<void*>(c), p, max_down_, max_up_, h);
    }

private:
    // The game's own pitch (before this frame's gyro write): where its stick took the view widens the gyro's limits.
    void widen_limits(double pitch) {
        max_up_ = std::max(max_up_, pitch);
        max_down_ = std::max(max_down_, -pitch);
    }

    bool control_ = false;
    bool aw2_ = false;  // Alan Wake 2's camera update is hooked
    bool cursor_ = false;
    control::Search search_ = control::Search::NotStarted;
    double max_up_ = 80, max_down_ = 80;  // degrees, widened by the game's own stick
};

}  // namespace

Engine& engine() {
    static NorthlightEngine e;
    return e;
}

}  // namespace gyro4all::northlight
