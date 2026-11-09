// RAGE (Rockstar Advanced Game Engine): Red Dead Redemption 2 (games/RedDeadRedemption2/MODDING_PLAN.md). The exe sits
// in the game's root folder next to its .rpf archives (common_0.rpf, shaders_x64.rpf), which detect the game. The exe's
// code is encrypted at rest (anti-tamper): no debugger. The game imports XInput, so the controller reaches it through the
// core's XInput translation (XInput=1).
//
// The camera: the gyro turns the active gameplay camera's stored front vector from this module's frame thread (no
// script hook needed; MODLOG 2026-11-03). Without a camera (loading), the gyro goes to the game as right-stick input
// (the core's blind stick fallback).
//
// The game thread (the one that pumps the sgaWindow) pumps window messages only now and then in the front end and
// menus (a few calls a second, or none for many seconds), so a gyro frame run from PeekMessageW left the translated
// controller frozen there. This module's own thread runs the frame instead, right after each camera update.
#include "rage.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

namespace gyro4all::rage {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

Engine::FrameFn g_frame = nullptr;

// What a camera update changes in the active camera's frame.
struct CameraPose {
    float forward[3], position[3];
};
bool camera_pose(CameraPose& pose);

// One gyro frame right after each game camera update, so every update takes exactly one turn: on a free-running 4 ms
// timer, 4 or 5 turns reached each 60 FPS update and the gyro's per-frame steps varied about 20% (the stick's 2%), seen
// as flicker on slow pans (MODLOG 2026-11-03). The update shows as a change of the active camera's frame forward row or
// position, polled every 1 ms: the first-person camera keeps the gyro's turn in that row, so an update can leave the row
// as it was and only move the position (MODLOG 2026-11-10). A second change within 6 ms is the same update; without one
// for 20 ms (still view, loading) the frame runs anyway.
DWORD WINAPI frame_thread(void*) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    log("rage: gyro frame thread %lu, after each camera update (%s timer)", GetCurrentThreadId(),
        timer ? "high-resolution" : "Sleep");
    LARGE_INTEGER freq, now, last_update{}, last_frame{};
    QueryPerformanceFrequency(&freq);
    const auto ms = [&](const LARGE_INTEGER& t) { return double(now.QuadPart - t.QuadPart) * 1000 / double(freq.QuadPart); };
    CameraPose seen{};
    for (;;) {
        LARGE_INTEGER due{};
        due.QuadPart = -10000;  // 1 ms, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        QueryPerformanceCounter(&now);
        CameraPose pose;
        bool run = false;
        if (camera_pose(pose) && memcmp(&pose, &seen, sizeof pose) != 0) {
            seen = pose;
            if (ms(last_update) >= 6) run = true;
            last_update = now;
        }
        if (!run && ms(last_frame) < 20) continue;
        last_frame = now;
        g_frame();
        if (camera_pose(pose)) seen = pose;  // the gyro's own write to a frame (first person, binoculars) isn't an update
    }
}

// The gameplay camera director (a global), its active camera, and the camera's stored front: a unit world vector the
// camera's orientation update (exe+0x421584, from the camera's vtable +0x248 call at exe+0x45bae5) turns into heading
// and pitch at the start of each update and writes back (exe+0x45bcae) after adding its input and modifiers.
constexpr uintptr_t kDirector = 0x3ea05e8, kActiveCamera = 0x7a0, kFront = 0x7b0;
// The camera the director blends from after a switch (the aim camera for ~1.5 s after releasing the aim, the on-foot one
// while aiming in). It no longer updates: its frame (rows right, forward, up at +0x20/+0x30/+0x40, then the position)
// stays where it was, so the blended view barely follows the gyro unless it is turned too.
constexpr uintptr_t kBlendFromCamera = 0x7a8, kFrame = 0x20;
constexpr double kPi = 3.14159265358979323846, kMaxPitch = 1.5;  // radians (86 deg)
// The rendered view's projection y scale, 1/tan(vertical FOV / 2), in a global render viewport copied every frame
// (MODLOG 2026-11-03): 2.2024 (48.9 deg) on foot, easing to 3.1716 (35.0 deg) aiming a rifle without a scope.
constexpr uintptr_t kProjectionY = 0x3ec0aa4;

// The rendered vertical FOV in degrees, 0 if unreadable or implausible.
// The offsets above are one RDR2 build's (1.0.1491.50; the Steam and Rockstar Games Launcher copies are the same):
// used only in RDR2.exe and while both known zoom-camera vtables (exe+0x3399308, exe+0x3399c08) start with a pointer
// into the exe's image. Another RAGE game (GTA IV) or another build gets no camera (the stick fallback) instead of writes
// to unrelated memory. The exe's data is unpacked a few seconds after start (anti-tamper): a check right at start failed
// on the Steam copy (MODLOG 2026-11-05), so a failed check is retried twice a second for as long as the game runs
// (the stick fallback meanwhile, so nothing is written), logged once after 60 s. A match is final.
bool build_ok() {
    static std::atomic<int> state{0};  // 1 = known build, -1 = not (logged), 0 = not yet
    static ULONGLONG first = 0, next = 0;
    if (state != 0) return state > 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return false;
    next = now + 500;
    if (!first) first = now;
    const HMODULE exe = GetModuleHandleW(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(exe, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const auto code_ptr = [&](uintptr_t vtable) {
        // Any alignment: a function's address needn't be 8-aligned (the Steam copy's aim vtable starts with exe+0x41e88c).
        const uintptr_t fn = vtable < end ? mem::rdp(vtable, 1) : 0;
        return fn >= base && fn < end;
    };
    const bool rdr2 = _wcsicmp(name, L"RDR2.exe") == 0;
    if (rdr2 && code_ptr(base + 0x3399308) && code_ptr(base + 0x3399c08)) {
        log("rage: %ls matches the known RDR2 build after %.1f s: camera offsets used", name, double(now - first) / 1000);
        state = 1;
    } else if (!rdr2) {
        log("rage: %ls is not the known RDR2 build: no camera hook (gyro as right stick)", name);
        state = -1;
    } else if (static bool warned = false; !warned && now - first >= 60000) {
        log("rage: %ls doesn't match the known RDR2 build yet after 60 s: gyro as right stick, still checking", name);
        warned = true;
    }
    return state > 0;
}

double vertical_fov() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    float y = 0;
    if (!build_ok() || !mem::rd(base + kProjectionY, y) || !std::isfinite(y) || y <= 0) return 0;
    const double fov = 2 * atan(1.0 / y) * 180 / kPi;
    return fov >= 5 && fov <= 170 ? fov : 0;
}

uintptr_t director() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return build_ok() ? mem::rdp(base + kDirector) : 0;
}

// The zoom cameras, by vtable (MODLOG 2026-11-03): aiming (exe+0x3399308) and the binoculars (exe+0x3399c08).
bool zoom_camera(uintptr_t cam) {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t vtable = cam ? mem::rdp(cam) : 0;
    return vtable == base + 0x3399308 || vtable == base + 0x3399c08;
}

uintptr_t active_camera() {
    const uintptr_t d = director();
    return d ? mem::rdp(d + kActiveCamera) : 0;
}

// The active camera's frame forward row and position, which the game writes once per camera update.
bool camera_pose(CameraPose& pose) {
    const uintptr_t cam = active_camera();
    return cam && mem::rd(cam + kFrame + 0x10, pose.forward) && mem::rd(cam + kFrame + 0x30, pose.position);
}

// The camera's front as heading (radians, grows to the left) and pitch (radians, up); false without a camera.
bool read_front(uintptr_t at, double& heading, double& pitch) {
    float f[3];
    if (!at || !mem::rd(at, f)) return false;
    const double len = sqrt(double(f[0]) * f[0] + double(f[1]) * f[1] + double(f[2]) * f[2]);
    if (!(fabs(len - 1.0) < 0.01)) return false;
    heading = atan2(-f[0], f[1]);
    pitch = asin(f[2] / len);
    return true;
}

// The front for heading h and pitch p, and the frame's right and up (no roll).
struct Axes {
    float right[3], forward[3], up[3];
};
Axes axes(double h, double p) {
    const double ch = cos(h), sh = sin(h), cp = cos(p), sp = sin(p);
    return {{float(ch), float(sh), 0}, {float(-sh * cp), float(ch * cp), float(sp)}, {float(sh * sp), float(-ch * sp), float(cp)}};
}

// Turns a camera's front by the gyro's degrees (yaw right, pitch up); with frame, also its frame's rotation rows (a
// camera that no longer updates).
bool turn_camera(uintptr_t cam, double pitch_deg, double yaw_deg, bool frame) {
    double h, p;
    if (!read_front(cam + kFront, h, p)) return false;
    h -= yaw_deg * kPi / 180.0;
    p = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, p + pitch_deg * kPi / 180.0));
    const Axes a = axes(h, p);
    if (!mem::guarded_store(cam + kFront, a.forward, sizeof a.forward)) return false;
    if (frame) {
        mem::guarded_store(cam + kFrame, a.right, sizeof a.right);
        mem::guarded_store(cam + kFrame + 0x10, a.forward, sizeof a.forward);
        mem::guarded_store(cam + kFrame + 0x20, a.up, sizeof a.up);
    }
    return true;
}

// Turns a camera's frame rows only (from its forward row).
bool turn_frame(uintptr_t cam, double pitch_deg, double yaw_deg) {
    double h, p;
    if (!read_front(cam + kFrame + 0x10, h, p)) return false;
    h -= yaw_deg * kPi / 180.0;
    p = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, p + pitch_deg * kPi / 180.0));
    const Axes a = axes(h, p);
    return mem::guarded_store(cam + kFrame, a.right, sizeof a.right) &&
           mem::guarded_store(cam + kFrame + 0x10, a.forward, sizeof a.forward) &&
           mem::guarded_store(cam + kFrame + 0x20, a.up, sizeof a.up);
}

// The active camera's view (heading, pitch as read_front): its stored front, or, for a camera without one (the
// binoculars' first-person camera, vtable exe+0x3399c08: its frame is rebuilt every frame from elsewhere, so writes to
// it don't stick), its frame's forward row. front = it has the stored front the gyro turns.
bool read_view(uintptr_t cam, double& heading, double& pitch, bool& front) {
    if (!cam) return false;
    front = read_front(cam + kFront, heading, pitch);
    return front || read_front(cam + kFrame + 0x10, heading, pitch);
}

// Turns the active camera, and the camera it blends from after a switch. A camera without a stored front gets the
// turn in its frame, which the game undoes: the stick fallback then sees it and learns that camera's stick speeds.
bool turn_front(double pitch_deg, double yaw_deg) {
    const uintptr_t d = director();
    const uintptr_t cam = d ? mem::rdp(d + kActiveCamera) : 0;
    double h, p;
    bool front;
    if (!read_view(cam, h, p, front)) return false;
    if (!front) return turn_frame(cam, pitch_deg, yaw_deg);
    if (!turn_camera(cam, pitch_deg, yaw_deg, false)) return false;
    if (const uintptr_t from = mem::rdp(d + kBlendFromCamera); from && from != cam)
        turn_camera(from, pitch_deg, yaw_deg, true);
    return true;
}

bool cursor_shown() {
    CURSORINFO ci{sizeof ci};
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
}

class RageEngine final : public Engine {
public:
    const char* name() const override { return "RAGE"; }

    // RDR2: <root>\<exe> with <root>\common_0.rpf and <root>\shaders_x64.rpf. GTA IV (and its episodes): <root>\<exe> with
    // the loose <root>\common\data and <root>\pc\data folders; its game exe is 32-bit, so only its 64-bit launcher can
    // load this DLL (is_launcher).
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return (exists(dir + L"\\common_0.rpf") && exists(dir + L"\\shaders_x64.rpf")) ||
               (exists(dir + L"\\common\\data") && exists(dir + L"\\pc\\data"));
    }

    // GTA IV's PlayGTAIV.exe starts the game.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring exe = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        return _wcsicmp(exe.c_str(), L"PlayGTAIV.exe") == 0;
    }

    const wchar_t* window_class() const override { return L"sgaWindow"; }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("rage: can't start the gyro frame thread (error %lu); the gyro frame stays on PeekMessageW", GetLastError());
        return false;
    }

    void configure() override { load_zoom_reference(); }
    void settings_changed() override { load_zoom_reference(); }

    // The unzoomed FOV for zoom compensation: the generic [Gyro4All] ZoomReferenceFOV= (horizontal degrees, profile or
    // Gyro4All.ini); 0 or implausible uses the FOV of the camera in use when it isn't a zoom camera. At start and on save.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_fov_)
            log("rage: zoom reference FOV %.1f degrees ([Gyro4All] ZoomReferenceFOV%s)", fov,
                fov > 0 ? "" : ": 0, the unzoomed camera's FOV");
        zoom_reference_fov_ = fov;
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    // The default view, held while a mouse cursor is shown; turned through the active camera's front, else (no camera:
    // loading) the gyro as right stick.
    EngineFrame read_frame() override {
        EngineFrame f;
        const bool was = cursor_;
        cursor_ = cursor_shown();
        if (cursor_ != was) log("rage: mouse cursor %s", cursor_ ? "shown (menu)" : "hidden");
        double h, p;
        bool front;
        const uintptr_t cam = active_camera();
        const bool camera = read_view(cam, h, p, front);
        if (camera != camera_)
            log("rage: gameplay camera %s", camera ? "found (gyro turns the camera)" : "gone (gyro as right stick)");
        camera_ = camera;
        f.cursor = cursor_;
        f.no_camera = !camera;
        f.in_game = true;
        f.view_id = 1;
        f.target = camera ? "gameplay camera front" : "no camera (gyro as right stick)";
        if (camera) {
            if (auto z = zoom_fov(cam)) f.zoom_fov = *z;
        }
        return f;
    }

    bool apply(double pitch, double yaw) override { return turn_front(pitch, yaw); }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        const uintptr_t cam = active_camera();
        double h, p;
        bool front;
        if (!read_view(cam, h, p, front)) return false;
        pitch = p * 180 / kPi;
        yaw = -h * 180 / kPi;
        camera = cam;  // the game makes a new camera object on each switch (on foot, aiming, binoculars, horse)
        return true;
    }

    // The current and unzoomed reference FOV, both vertical degrees, for GyroLib's zoom compensation: the rendered FOV,
    // against ZoomReferenceFOV= (converted with the window's aspect) or else the FOV of the last camera that isn't a zoom
    // camera (aim, binoculars), so the on-foot, horse and first-person cameras each count as unzoomed at their own FOV.
    // The widest FOV seen was wrong as a reference: a 90-degree frame when the camera appears after a load halved the
    // gyro everywhere (MODLOG 2026-11-03).
    std::optional<ZoomFov> zoom_fov(uintptr_t cam) {
        const double current = vertical_fov();
        if (current <= 0) return std::nullopt;
        double reference;
        if (zoom_reference_fov_ > 0) {
            reference = fov::vertical_from_horizontal(zoom_reference_fov_, fov::game_window_aspect());
        } else {
            const bool zoom = zoom_camera(cam);
            if (zoom && !zoom_camera_)
                log("rage: zoom camera (aim or binoculars): unzoomed FOV %.1f degrees vertical", unzoomed_fov_);
            zoom_camera_ = zoom;
            if (!zoom) unzoomed_fov_ = current;
            reference = unzoomed_fov_;
        }
        if (!fov::is_valid(reference)) return std::nullopt;
        return ZoomFov{current, reference};
    }

    // Only the mouse cursor tells menus apart: GyroLib's "menus only" automatic calibration would never run.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (cursor_) return "view=menu (cursor)";
        return camera_ ? "view=default (camera front)" : "view=default (gyro as right stick)";
    }

private:
    bool cursor_ = false;
    bool camera_ = false;
    double zoom_reference_fov_ = 0;  // [Gyro4All] ZoomReferenceFOV=, horizontal degrees; 0 = the unzoomed camera's
    double unzoomed_fov_ = 0;        // vertical degrees, of the last camera that isn't a zoom camera
    bool zoom_camera_ = false;
};

}  // namespace

Engine& engine() {
    static RageEngine e;
    return e;
}

}  // namespace gyro4all::rage
