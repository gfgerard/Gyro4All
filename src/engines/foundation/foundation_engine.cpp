// Foundation (Crystal Dynamics / Eidos Montreal, `cdc::` classes): Shadow of the Tomb Raider and Rise of the Tomb Raider
// (MODLOG 2026-11-19/20). Native
// x64, MSVC RTTI kept, Z up, DX11/DX12 loaded at run time, data in bigfile.*.tiger next to the exe.
//
// The gyro frame: the game's window is pumped by a thread of its own with a blocking GetMessageW loop (SOTTR.exe RVA
// 0xfd2950); its only PeekMessageW call (RVA 0xfb5dd5) is a loop that runs only while a state flag is set. So the core's
// end-of-pump frame never runs. This module's own thread runs it instead, woken by each Present of the game's swapchain
// (overlay::on_present), with a timer when nothing presents (loading, or the panel's graphics hooks off): one stable
// thread for GyroLib, once per rendered frame.
//
// The cameras: the game switches between ICameraController classes (Exploration, Aim, Stealth, Free, PhotoMode...).
// Two are turned, each by its yaw (radians; the Exploration controller wraps it to [0, 2pi); it decreases turning right) and pitch
// (radians; + looks down in the Exploration controller), the sources: the game keeps a write and the view follows
// (write tests in game, MODLOG 2026-11-20). Each frame the game rebuilds a copy of each in the controller in use only.
//   ExplorationCameraController (third person): yaw +0x12D8, pitch +0x12F0, copies +0x12D4 / +0x12EC. Several exist.
//   AimCameraController (bow and weapons): yaw +0x908, pitch +0x920, copies +0x904 / +0x91C. Made while aiming (often
//   a new object each time).
// ROTTR has the same controller classes and code with other offsets (its functions match SOTTR's instruction for
// instruction, MODLOG 2026-11-20): Exploration yaw +0x1020, pitch +0x1038, copies +0x101C / +0x1034; Aim yaw +0x8E8,
// pitch +0x900, copies +0x8E4 / +0x8FC; the start function is slot 6 (its vtables have one slot fewer before it). The
// offsets below are SOTTR's; kLayouts has both games'.
// Slot 7 of each controller vtable (0x8d0450 for ExplorationCameraController in the 2023 Epic build) sets a controller's
// angles from the previous camera's ([rdx+0x2c]) when the game (re)starts it: hooked, it lists the controllers. It runs
// on more than one per switch, and the last one isn't always the one in use. So the kind of camera is the kind slot 7
// ran for last (Aim when the bow is drawn, Exploration again when it is lowered), and within that kind the controller is
// the one whose copies changed last (an Aim controller only after its own slot 7), else the one slot 7 ran on. The copies
// alone aren't enough: an aimed bow held still changes none, and the Exploration copies keep changing behind it.
// Before any Exploration copy changes (a still camera after a load) the gyro's turn goes to every Exploration controller
// seen: only the one in use rebuilds its copies from it, so it is picked on the next frame; the game sets the others'
// angles anew when it switches to them (slot 7), so the turn they took doesn't stay.
//
// The FOV: the render camera struct (the layout Otis_Inf's IGCS camera uses: position +0x80, quaternion +0xA0, vertical
// FOV +0xB0 in radians) is the second argument of the function IGCS hooks for the camera address (SOTTR.exe+0x495b30 in
// the 2023 Epic build, found by IGCS's pattern 0x1f bytes into it); hooked, it gives the struct. Its FOV is the view's,
// also while aiming: 55 degrees on foot (up to about 61 looking up), 35 with the bow drawn, 15 with its extra zoom. (The
// Exploration controller's own render block at +0x1850, FOV +0x1900, held it in one session and 0 in the next.) The
// unzoomed reference is the Exploration controller's configured FOV +0x324 (radians, 55 degrees; it didn't change in
// any recording); ZoomReferenceFOV= overrides it. The FOV reported is never wider than that, so looking up doesn't speed
// the gyro up.
#include "foundation.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "overlay.hpp"
#include "rtti.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gyro4all::foundation {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

constexpr double kPi = 3.14159265358979323846;
constexpr uintptr_t kRenderFov = 0xB0;    // render camera struct: the view's vertical FOV, float radians
// IGCS's camera-address pattern, kCameraFnOffset bytes into the function whose rdx is the render camera struct.
constexpr const char* kCameraPattern = "89 59 28 45 33 C0 0F 28 82 80 00 00 00 4C 8B F9 41 0F B6 D1";
constexpr uintptr_t kCameraFnOffset = 0x1F;
constexpr double kMaxPitch = 1.40;     // the gyro keeps the pitch within this (80 degrees)

// --- the gyro frame ----------------------------------------------------------------------------------------------

Engine::FrameFn g_frame = nullptr;
HANDLE g_present_event = nullptr;
std::atomic<ULONGLONG> g_last_present{0};

void on_present() {
    g_last_present.store(GetTickCount64(), std::memory_order_relaxed);
    SetEvent(g_present_event);
}

DWORD WINAPI frame_thread(void*) {
    log("foundation: gyro frame thread %lu, woken by the game's Present (8 ms timer without it)", GetCurrentThreadId());
    for (;;) {
        const bool presenting = GetTickCount64() - g_last_present.load(std::memory_order_relaxed) < 1000;
        WaitForSingleObject(g_present_event, presenting ? 50 : 8);
        g_frame();
    }
}

// --- the camera controllers --------------------------------------------------------------------------------------

struct Controller {
    const char* rtti;
    const char* label;
    uintptr_t vtable = 0;
    std::atomic<uint32_t> calls{0};  // slot 7 calls since the last status line
};
Controller g_controllers[] = {
    {".?AVExplorationCameraController@@", "exploration"}, {".?AVAimCameraController@@", "aim"},
    {".?AVStealthCameraController@@", "stealth"},         {".?AVFreeCameraController@@", "free"},
    {".?AVPhotoModeCameraController@@", "photo mode"},    {".?AVSplineCameraController@@", "spline"},
    {".?AVAnchoredCameraController@@", "anchored"},       {".?AVCameraBoneCameraController@@", "camera bone"},
    {".?AVForceCameraController@@", "force"},             {".?AVSimpleCameraController@@", "simple"},
};
constexpr int kControllers = int(std::size(g_controllers));

// Eight arguments: the four in registers and four stack slots copied through unchanged, so a slot 7 that takes more
// than four (or floats, which no code here touches) gets what its caller passed.
using ActivateFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
ActivateFn g_activate_orig[kControllers] = {};

// The cameras the gyro turns.
struct Kind {
    const char* label;
    int controller;  // index in g_controllers
    uint32_t view;
    uintptr_t yaw, pitch, yaw_copy, pitch_copy;
};
constexpr int kKindCount = 2;
constexpr int kExplorationKind = 0, kAimKind = 1;

// Per game, picked by the exe's name.
struct Layout {
    const wchar_t* exe;
    Kind kinds[kKindCount];
    int activate_slot;   // the controllers' start function in their vtables
    uintptr_t base_fov;  // Exploration controller: its configured (unzoomed) vertical FOV, radians; 0: not known
    // The render camera struct, a global in the exe (0: found through the render camera function instead).
    uintptr_t render_camera;
};
constexpr Layout kLayouts[] = {
    {L"sottr.exe", {{"exploration", 0, 1, 0x12D8, 0x12F0, 0x12D4, 0x12EC}, {"aim", 1, 2, 0x908, 0x920, 0x904, 0x91C}}, 7,
     0x324, 0},
    {L"rottr.exe", {{"exploration", 0, 1, 0x1020, 0x1038, 0x101C, 0x1034}, {"aim", 1, 2, 0x8E8, 0x900, 0x8E4, 0x8FC}}, 6,
     0x330, 0x145CAB0},
};
const Layout* g_layout = nullptr;  // set in configure(), before any hook
const Kind* kKinds = nullptr;      // g_layout->kinds

// Controllers seen by slot 7 (as this or an argument), per kind: a ring, so the Aim objects made at each aim replace
// the oldest ones.
constexpr int kMaxCandidates = 16;
struct Candidates {
    std::atomic<uintptr_t> obj[kMaxCandidates] = {};
    std::atomic<uint32_t> added{0};
};
Candidates g_candidates[kKindCount];

// The kind slot 7 ran for last (-1: none yet), the object it ran on, and when.
std::atomic<int> g_activated_kind{-1};
std::atomic<uintptr_t> g_activated[kKindCount] = {};
std::atomic<ULONGLONG> g_activated_at[kKindCount] = {};

bool is_kind(uintptr_t obj, int kind) {
    uintptr_t vt = 0;
    return obj && mem::plausible(obj) && mem::rd(obj, vt) && vt == g_controllers[kKinds[kind].controller].vtable;
}

// Any thread (the game's, in the hook).
void note_candidate(uintptr_t obj) {
    for (int k = 0; k < kKindCount; ++k) {
        if (!is_kind(obj, k)) continue;
        Candidates& c = g_candidates[k];
        for (auto& o : c.obj)
            if (o.load(std::memory_order_relaxed) == obj) return;
        const uint32_t at = c.added.fetch_add(1, std::memory_order_acq_rel);
        c.obj[at % kMaxCandidates].store(obj, std::memory_order_release);
        if (at < 20)
            log("foundation: %s camera controller %s seen (%u so far)", kKinds[k].label, mem::module_offset(obj).c_str(),
                at + 1);
        return;
    }
}

template <int K>
void* hooked_activate(void* self, void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    g_controllers[K].calls.fetch_add(1, std::memory_order_relaxed);
    for (void* obj : {self, a, b, c}) note_candidate(reinterpret_cast<uintptr_t>(obj));
    for (int k = 0; k < kKindCount; ++k)
        if (kKinds[k].controller == K) {
            g_activated[k].store(reinterpret_cast<uintptr_t>(self), std::memory_order_release);
            g_activated_at[k].store(GetTickCount64(), std::memory_order_release);
            g_activated_kind.store(k, std::memory_order_release);
        }
    return g_activate_orig[K](self, a, b, c, d, e, f, g);
}

template <int... K>
constexpr void* hook_of(int k, std::integer_sequence<int, K...>) {
    void* hooks[] = {reinterpret_cast<void*>(&hooked_activate<K>)...};
    return hooks[k];
}

void hook_controllers() {
    const int slot = g_layout->activate_slot;
    const rtti::Image im = rtti::exe_image();
    std::string missing;
    for (int k = 0; k < kControllers; ++k) {
        Controller& c = g_controllers[k];
        c.vtable = rtti::primary_vtable(im, c.rtti);
        void* original = c.vtable ? rtti::patch_slot(c.vtable, slot,
                                                     hook_of(k, std::make_integer_sequence<int, kControllers>{}))
                                  : nullptr;
        if (!original) {
            missing += missing.empty() ? c.label : std::string(", ") + c.label;
            continue;
        }
        g_activate_orig[k] = reinterpret_cast<ActivateFn>(original);
    }
    log("foundation: camera controllers: ExplorationCameraController vtable %s, slot %d %s; AimCameraController %s%s%s",
        g_controllers[0].vtable ? mem::module_offset(g_controllers[0].vtable).c_str() : "not found", slot,
        g_activate_orig[0] ? "hooked" : "NOT hooked", g_activate_orig[1] ? "hooked" : "NOT hooked",
        missing.empty() ? "" : "; not hooked: ", missing.c_str());
}

// --- the render camera (for the FOV) -------------------------------------------------------------------------------

using CameraFn = void* (*)(void*, void*, void*, void*, void*, void*, void*, void*);
CameraFn g_camera_fn_orig = nullptr;
std::atomic<uintptr_t> g_render_camera{0};

void* hooked_camera_fn(void* self, void* camera, void* a, void* b, void* c, void* d, void* e, void* f) {
    g_render_camera.store(reinterpret_cast<uintptr_t>(camera), std::memory_order_release);
    return g_camera_fn_orig(self, camera, a, b, c, d, e, f);
}

void hook_render_camera() {
    if (g_layout->render_camera) {
        const uintptr_t camera = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + g_layout->render_camera;
        g_render_camera.store(camera, std::memory_order_release);
        log("foundation: render camera %s (the FOV for zoom compensation)", mem::module_offset(camera).c_str());
        return;
    }
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(kCameraPattern, bytes, mask);
    const auto hits = mem::find_pattern(mem::code_sections(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                                        bytes.data(), mask.data(), bytes.size());
    // The function's start: mov [rsp+8], rcx; push rbp; push rbx.
    static const uint8_t kStart[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53};
    uint8_t start[sizeof kStart] = {};
    const uintptr_t fn = hits.size() == 1 ? hits[0] - kCameraFnOffset : 0;
    if (!fn || !mem::guarded_copy(start, fn, sizeof start) || std::memcmp(start, kStart, sizeof start) != 0) {
        log("foundation: render camera function not found (%zu pattern matches): no FOV for zoom compensation",
            hits.size());
        return;
    }
    if (!hooks::hook_function(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&hooked_camera_fn),
                              reinterpret_cast<void**>(&g_camera_fn_orig))) {
        log("foundation: render camera function %s: hook failed; no FOV for zoom compensation", mem::module_offset(fn).c_str());
        return;
    }
    log("foundation: render camera function %s hooked (the FOV for zoom compensation)", mem::module_offset(fn).c_str());
}

// --- the camera in use (gyro frame thread only) ------------------------------------------------------------------

struct Seen {
    uintptr_t obj = 0;
    float yaw = 0, pitch = 0;
    ULONGLONG changed = 0;  // GetTickCount64 of the last change of its copies (0: not seen changing)
};
Seen g_seen[kKindCount][kMaxCandidates];
uintptr_t g_in_use[kKindCount] = {};  // per kind, the candidate whose copies changed last
ULONGLONG g_in_use_changed[kKindCount] = {};
int g_kind = -1;  // the camera turned: kExplorationKind, kAimKind, or -1 (none yet)

void follow_cameras() {
    const ULONGLONG now = GetTickCount64();
    for (int k = 0; k < kKindCount; ++k) {
        const Kind& kind = kKinds[k];
        for (int i = 0; i < kMaxCandidates; ++i) {
            const uintptr_t obj = g_candidates[k].obj[i].load(std::memory_order_acquire);
            Seen& s = g_seen[k][i];
            float y = 0, p = 0;
            if (!is_kind(obj, k) || !mem::rd(obj + kind.yaw_copy, y) || !mem::rd(obj + kind.pitch_copy, p)) {
                s = {};
                continue;
            }
            if (s.obj != obj) s = {obj, y, p, 0};
            else if (y != s.yaw || p != s.pitch) s = {obj, y, p, now};
            if (s.changed && s.changed >= g_in_use_changed[k]) {
                static int logs = 0;
                if (g_in_use[k] != obj && logs < 40 && ++logs)
                    log("foundation: the %s camera in use is %s (its angles changed)%s", kind.label,
                        mem::module_offset(obj).c_str(), logs == 40 ? "; further changes not logged" : "");
                g_in_use[k] = obj;
                g_in_use_changed[k] = s.changed;
            }
        }
        if (g_in_use[k] && !is_kind(g_in_use[k], k)) g_in_use[k] = 0;  // freed (an Aim object after aiming)
    }
    // Aim: the controller whose copies changed since the bow was drawn, else the one slot 7 ran on.
    const ULONGLONG aimed = g_activated_at[kAimKind].load(std::memory_order_acquire);
    uintptr_t aim = g_in_use_changed[kAimKind] >= aimed ? g_in_use[kAimKind] : 0;
    if (!aim) {
        const uintptr_t started = g_activated[kAimKind].load(std::memory_order_acquire);
        if (is_kind(started, kAimKind)) aim = started;
    }
    const int activated = g_activated_kind.load(std::memory_order_acquire);
    int kind = activated == kAimKind && aim ? kAimKind : g_in_use[kExplorationKind] ? kExplorationKind : -1;
    if (kind == kAimKind && g_in_use[kAimKind] != aim) {
        g_in_use[kAimKind] = aim;
        g_in_use_changed[kAimKind] = aimed;
    }
    if (kind != g_kind && kind >= 0)
        log("foundation: turning the %s camera %s", kKinds[kind].label, mem::module_offset(g_in_use[kind]).c_str());
    g_kind = kind;
}

// Turns one controller of `kind` (radians already converted to the game's senses: yaw - right, pitch + down).
bool turn(uintptr_t c, int kind, double yaw_rad, double pitch_rad) {
    const Kind& k = kKinds[kind];
    float y = 0, p = 0;
    if (!is_kind(c, kind) || !mem::rd(c + k.yaw, y) || !mem::rd(c + k.pitch, p) || !std::isfinite(y) || !std::isfinite(p))
        return false;
    // Not wrapped here: the Exploration controller wraps its yaw itself, and a yaw this code wrapped jumped by 2pi for
    // the game's own easing toward a target (guns: the view swung to one side and back on some shots).
    const float out_yaw = float(double(y) + yaw_rad);
    const float out_pitch = float(std::clamp(double(p) + pitch_rad, -kMaxPitch, kMaxPitch));
    return mem::guarded_store(c + k.yaw, &out_yaw, sizeof out_yaw) &&
           mem::guarded_store(c + k.pitch, &out_pitch, sizeof out_pitch);
}

// The controllers of one kind whose copies changed in the last kLiveMs: in ROTTR two rebuild them every frame (two
// Exploration ones, and two Aim ones while aiming), and turning only the one that changed last took turns between them
// (jitter: half the gyro's turn on each). All of them are turned. With none changed lately (a still camera: after a load
// the game may also have changed another one once, ROTTR), an Exploration turn goes to every known one, as before the
// first pick: the one in use rebuilds its copies from it and is the only one turned from the next frame. An Aim turn
// then goes to the one in use only.
constexpr ULONGLONG kLiveMs = 200;
bool turn_live(int kind, double yaw_rad, double pitch_rad) {
    const ULONGLONG now = GetTickCount64();
    bool any = false;
    for (const Seen& s : g_seen[kind])
        if (s.obj && s.changed && now - s.changed <= kLiveMs) any = turn(s.obj, kind, yaw_rad, pitch_rad) || any;
    if (any || kind != kExplorationKind) return any;
    for (auto& o : g_candidates[kExplorationKind].obj)
        any = turn(o.load(std::memory_order_acquire), kExplorationKind, yaw_rad, pitch_rad) || any;
    return any;
}

// No Exploration controller picked yet, but some are known: the turn goes to all of them (see the top of the file).
bool probing() { return g_kind < 0 && g_candidates[kExplorationKind].added.load(std::memory_order_acquire) > 0; }

// The camera turned, with readable, finite angles (0 if none).
uintptr_t camera(float& yaw, float& pitch) {
    if (g_kind < 0) return 0;
    const Kind& kind = kKinds[g_kind];
    const uintptr_t c = g_in_use[g_kind];
    if (!is_kind(c, g_kind) || !mem::rd(c + kind.yaw, yaw) || !mem::rd(c + kind.pitch, pitch) || !std::isfinite(yaw) ||
        !std::isfinite(pitch))
        return 0;
    return c;
}

class FoundationEngine final : public Engine {
public:
    const char* name() const override { return "Foundation"; }

    // The game's data: bigfile.000.tiger next to the exe (Shadow of the Tomb Raider; Rise of the Tomb Raider too).
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\bigfile.000.tiger");
    }

    void configure() override {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring exe = path;
        exe = exe.substr(exe.find_last_of(L"\\/") + 1);
        for (wchar_t& ch : exe) ch = wchar_t(towlower(ch));
        g_layout = &kLayouts[0];
        for (const Layout& l : kLayouts)
            if (exe == l.exe) g_layout = &l;
        kKinds = g_layout->kinds;
        if (exe != g_layout->exe) log("foundation: unknown game exe: Shadow of the Tomb Raider's camera offsets used");
        hook_controllers();
        hook_render_camera();
        load_zoom_reference();
    }

    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Exploration", 0, true}, {2, "Aim", 1, true}}; }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        g_present_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_present_event) {
            log("foundation: can't create the Present event (error %lu); no gyro frame", GetLastError());
            return false;
        }
        overlay::on_present(&on_present);
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("foundation: can't start the gyro frame thread (error %lu); no gyro frame", GetLastError());
        overlay::on_present(nullptr);
        return false;
    }

    EngineFrame read_frame() override {
        follow_cameras();
        EngineFrame f;
        float yaw = 0, pitch = 0;
        if (camera(yaw, pitch)) {
            f.in_game = true;
            f.view_id = kKinds[g_kind].view;
            f.target = std::string(kKinds[g_kind].label) + " camera";
            f.zoom_fov = zoom_fov();
        } else if (probing()) {
            f.in_game = true;
            f.view_id = kKinds[kExplorationKind].view;
            f.target = "exploration camera (finding the one in use)";
        } else {
            f.target = "no camera yet";
        }
        return f;
    }

    // GyroLib's degrees: pitch + up, yaw + right. The game's: pitch + down, yaw decreasing to the right.
    bool apply(double pitch, double yaw) override {
        const double yaw_rad = -yaw * kPi / 180, pitch_rad = -pitch * kPi / 180;
        if (probing()) {
            bool any = false;
            for (auto& o : g_candidates[kExplorationKind].obj)
                any = turn(o.load(std::memory_order_acquire), kExplorationKind, yaw_rad, pitch_rad) || any;
            return any;
        }
        float y = 0, p = 0;
        const uintptr_t c = camera(y, p);
        if (c && turn_live(g_kind, yaw_rad, pitch_rad)) return true;
        return c && turn(c, g_kind, yaw_rad, pitch_rad);
    }

    // No menus or pauses are read yet: GyroLib's menus-only calibration would never run.
    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (g_kind < 0) return "foundation: no camera in use yet";
        return std::format("foundation: {} camera {} (exploration {} seen, aim {} seen)", kKinds[g_kind].label,
                           mem::module_offset(g_in_use[g_kind]), g_candidates[kExplorationKind].added.load(),
                           g_candidates[kAimKind].added.load());
    }

    // How often each controller's slot 7 ran: once per switch, or every frame.
    void status_details() override {
        std::string line;
        for (Controller& c : g_controllers)
            if (const uint32_t n = c.calls.exchange(0, std::memory_order_relaxed))
                line += std::format(" {} {}", c.label, n);
        if (!line.empty())
            log("foundation: camera controller slot %d calls since the last status:%s", g_layout->activate_slot, line.c_str());
    }

private:
    double zoom_reference_ = 0;     // ZoomReferenceFOV= (horizontal degrees), 0: the Exploration controller's base_fov
    double logged_fov_ = 0;
    int fov_logs_ = 0;

    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_ || fov_logs_ == 0)
            log("foundation: zoom reference FOV %s", fov > 0 ? (std::to_string(int(fov)) + " degrees horizontal").c_str()
                                                             : "the exploration camera's configured FOV");
        zoom_reference_ = fov;
    }

    // The render camera's FOV (the view's, also while aiming) against the unzoomed one, never wider than it.
    std::optional<ZoomFov> zoom_fov() {
        float rad = 0, base = 0;
        const uintptr_t e = g_in_use[kExplorationKind];
        const uintptr_t camera = g_render_camera.load(std::memory_order_acquire);
        if (!mem::plausible(camera, 16) || !mem::rd(camera + kRenderFov, rad) || !is_kind(e, kExplorationKind) ||
            (zoom_reference_ <= 0 && (!g_layout->base_fov || !mem::rd(e + g_layout->base_fov, base))))
            return std::nullopt;
        ZoomFov z;
        z.reference_vertical = zoom_reference_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_, fov::game_window_aspect())
                                   : double(base) * 180 / kPi;
        const double current = double(rad) * 180 / kPi;
        if (!fov::is_valid(current) || !fov::is_valid(z.reference_vertical)) return std::nullopt;
        z.current_vertical = std::min(current, z.reference_vertical);
        if (std::fabs(current - logged_fov_) > 1 && fov_logs_ < 50) {
            ++fov_logs_;
            logged_fov_ = current;
            log("foundation: FOV %.1f degrees vertical (unzoomed %.1f, %s camera)", z.current_vertical,
                z.reference_vertical, kKinds[g_kind].label);
        }
        return z;
    }
};

}  // namespace

Engine& engine() {
    static FoundationEngine instance;
    return instance;
}

}  // namespace gyro4all::foundation
