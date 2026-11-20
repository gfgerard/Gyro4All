// Dawn engine (Eidos Montréal's fork of Glacier 2). Native x64, MSVC RTTI kept, world Z up. Detected by the layout
// (dawn_layout), the camera route chosen by the exe's RTTI classes (configure). Two games so far:
//
// Deus Ex: Mankind Divided (GOG, retail\DXMD.exe 1.19.0.0; MODLOG 2026-11-15, games/DeusExMankindDivided/MODDING_PLAN.md).
// The camera's source is the player character's own yaw and pitch: ZPlayerCharacter +0xB28 (yaw) and +0xB2C (pitch,
// positive looks down: tested in game), float radians. The game builds the view from them and keeps a write there (write tests). The render camera
// (the player's ZGenericCamera) is a copy rebuilt every frame: rotation rows +0x50/+0x5C/+0x68 (the third row is
// (-sin yaw cos pitch, -cos yaw cos pitch, sin pitch), the camera's back: +pitch raises it and lowers the view), position +0x74, vertical FOV +0x13C in degrees.
// The gyro turns the angles only while the camera's forward matches them: in cutscenes, takedowns, cover (third person)
// and other scripted cameras it doesn't, and the gyro waits.
//
// Marvel's Guardians of the Galaxy (Epic, retail\gotg.exe, Denuvo; games/MarvelGOTG/MODDING_PLAN.md). Third person.
// From the game's own PDB: the camera modes' ZCameraBehavior_AimRotation (and its _Offset subclass) keep the rotation
// they built the shot from in m_degYaw +0xF0 / m_degPitch +0xF4 (float degrees; ZCameraBehavior_Rotation::UpdateInternal
// clamps and stores them each update), m_bIsActive +0x20, m_pPlayerController +0x180 (the player's behaviours have one).
// The behaviours are made and freed with their camera modes (aiming, sprint, combat), so they are read every frame from
// the player's camera, the ZPlayerCameraSystem's m_CameraEntity (+0x48, a ZCameraEntity): m_modeStack +0x510 and
// m_modeOverrides +0x520 list its ZCameraModes, each with m_apBehaviors +0x40. The gyro turns every active player
// behaviour (more than one only during a blend). The camera: world matrix +0x60 (3x4 floats), vertical FOV +0x140
// degrees, m_bIsPaused +0x59B. Writes stick and the senses are DXMD's (in game, DualSense).
//
// The frame: neither game pumps its game window with PeekMessage on the game thread once per frame (DXMD: GetMessageW on
// a thread of its own; GOTG: PeekMessage about once a second), so the core's pump hook sees no game frames.
// Gyro4All's frame runs on a thread of its own, right after each update of the player camera (as RAGE's for RDR2,
// MODLOG 2026-11-03/10): the thread polls the camera's pose every 1 ms, and a change is a camera update, so every update
// takes exactly one turn. A second change within 2.5 ms is the same update (the game writes the whole matrix at once and
// Gyro4All never writes it; RDR2's 6 ms merged a third of DXMD's ~6 ms frames at 160 FPS, MODLOG 2026-11-15); without
// one for 20 ms (no player camera, a still view, loading) the frame runs anyway.
//
// Objects are found by their vtables (from the RTTI names, so not tied to one build) with a heap scan on a thread of its own.
#include "dawn.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "rtti.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gyro4all::dawn {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr ULONGLONG kRescanMs = 2000;

// Dawn games keep their resources in <root>\runtime\ as <hash>.pc_headerlib/.pc_resourcelib files (Glacier 2's
// format; HITMAN 3's Glacier keeps .rpkg packages), next to <root>\retail\<exe>.
bool dawn_layout(const std::wstring& exe_path) {
    const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\..\\runtime\\*.pc_headerlib").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

// Which camera classes the exe has (its RTTI), not its name, picks the route:
//   Character: ZPlayerCharacter + ZGenericCamera (DXMD): the character's yaw/pitch, matched against the render camera.
//   Behaviour: ZCameraBehavior_AimRotation + ZPlayerCameraSystem (GOTG's camera modes): the active aim behaviours.
enum class Route { None, Character, Behaviour };

// --- Character route (DXMD) -----------------------------------------------------------------------------------
constexpr uintptr_t kYaw = 0xB28;                   // in ZPlayerCharacter: yaw, pitch, float radians
constexpr uintptr_t kForward = 0x68, kFov = 0x13C;  // in ZGenericCamera: forward row (3 floats), vertical FOV degrees
// The gyro keeps the pitch within this (radians, 80 degrees); the game's own limit is unknown.
constexpr double kMaxPitch = 1.4;
// Yaw sense: +1 when the game's yaw grows turning right (a +yaw write lowered the forward's heading atan2(y, x), which
// is a clockwise turn seen from above in a right-handed Z-up world).
constexpr double kYawSense = 1;
// Pitch sense: the game's pitch grows looking down (in game, DualSense; the write test only saw the camera row rise).
constexpr double kPitchSense = -1;
// The camera's forward and the angles agree within this (cosine of about 8 degrees) in gameplay.
constexpr double kMatchCos = 0.99;
// A mismatch between the camera and the angles must last this long before the gyro stops (another camera).
constexpr ULONGLONG kMismatchMs = 150;

// --- Behaviour route (GOTG) -----------------------------------------------------------------------------------
constexpr uintptr_t kBehActive = 0x20, kBehDeg = 0xF0, kBehController = 0x180;  // ZCameraBehavior_AimRotation
constexpr uintptr_t kBehShot = 0x40, kBehShotSize = 0x80;  // m_backupShot: the input shot, copied in each update
constexpr uintptr_t kSysCamera = 0x48, kSysFreeCam = 0x90;  // ZPlayerCameraSystem: the ZCameraEntity*, free camera
// ZCameraEntity (a ZRenderCameraEntity): world matrix, vertical FOV degrees, its camera modes, paused.
constexpr uintptr_t kRcWorld = 0x60, kRcFov = 0x140;
constexpr uintptr_t kCamModeStack = 0x510;      // TArray<TPair<TSharedPtr<ZCameraMode>, status>>: 24-byte entries
constexpr uintptr_t kCamModeOverrides = 0x520;  // TArray<TSharedPtr<ZCameraMode>>: 16-byte entries
constexpr uintptr_t kCamPaused = 0x59B;
constexpr uintptr_t kModeBehaviours = 0x40;  // ZCameraMode: TArray<TSharedPtr<ZCameraBehavior>>
constexpr uintptr_t kModeCamera = 0x28;      // ZCameraMode: m_pCamera's ZCameraEntity* (TEntityRefBase +8)
constexpr uintptr_t kBehMode = 0x28;         // ZCameraBehavior: m_mode (ZCameraMode*)
constexpr double kGotgMaxPitch = 80;  // degrees; the game clamps further on its next update
// Yaw grows turning right, pitch grows looking down (as DXMD; right in game, DualSense).
constexpr double kGotgYawSense = 1, kGotgPitchSense = -1;
constexpr size_t kMaxActive = 4;

// --- the frame ----------------------------------------------------------------------------------------------------

Engine::FrameFn g_frame = nullptr;
// What a camera update changes (0: nothing watched), set by read_frame; a change in either is an update. DXMD: the
// camera's third row and position (+0x68, 24 bytes). GOTG: the active aim behaviour's input shot (written in the same
// update as its angles, which Gyro4All writes and so aren't watched) and the camera's world matrix.
constexpr size_t kWatchMax = 0x80;
std::atomic<uintptr_t> g_watch[2]{};
std::atomic<size_t> g_watch_size[2]{};

struct Pose {
    unsigned char bytes[2][kWatchMax];
};

bool watched_pose(Pose& pose) {
    std::memset(&pose, 0, sizeof pose);
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        const uintptr_t at = g_watch[i].load(std::memory_order_relaxed);
        const size_t n = g_watch_size[i].load(std::memory_order_relaxed);
        if (at && n && n <= kWatchMax && mem::guarded_copy(pose.bytes[i], at, n)) any = true;
    }
    return any;
}

// --- GOTG: the camera update hook ------------------------------------------------------------------------------------
// ZCameraBehavior_Rotation::UpdateInternal (vtable slot 8 of ZCameraBehavior_AimRotation and of _Offset, which has its
// own): void(this, ZCameraShot* in, ZCameraShot* out, SCameraModeUpdateInfo*, float dt). It runs on the game's job
// worker threads (a different one almost every frame), so Gyro4All's frame stays on its own thread: the first update
// of each frame wakes it (exact update times instead of a 1 ms poll), and each update adds the turn the frame thread
// left pending to its own behaviour's angles before the game reads them (no write lands mid-update). The vtable slot is
// a data pointer (no code patched under Denuvo).
constexpr int kUpdateSlot = 8;
using UpdateFn = void(__fastcall*)(void*, void*, void*, void*, float);
UpdateFn g_update_orig[2] = {};
bool g_hooked = false;
HANDLE g_update_event = nullptr;          // auto-reset: the first behaviour update of a frame
std::atomic<long long> g_hook_last{0};    // QPC time of that update (0: none yet)
std::atomic<bool> g_turn_live{false};     // the frame thread's verdict: turn the player's aim behaviours
SRWLOCK g_turn_lock = SRWLOCK_INIT;
double g_pending[2] = {};                 // degrees (yaw, pitch) for the next update, from the frame thread
double g_frame_turn[2] = {};              // this frame's turn, taken from g_pending by its first update
long long g_qpc_freq = 1;

void on_behaviour_update(void* self) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long last = g_hook_last.load(std::memory_order_relaxed);
    // The first update of a frame (2.5 ms after the previous first one) takes the pending turn and wakes the frame
    // thread; the updates on other workers in the same frame use the same turn.
    if ((!last || (now.QuadPart - last) * 1000 >= g_qpc_freq * 5 / 2) &&
        g_hook_last.compare_exchange_strong(last, now.QuadPart, std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_turn_lock);
        g_frame_turn[0] = g_pending[0];
        g_frame_turn[1] = g_pending[1];
        g_pending[0] = g_pending[1] = 0;
        ReleaseSRWLockExclusive(&g_turn_lock);
        SetEvent(g_update_event);
    }
    if (!g_turn_live.load(std::memory_order_relaxed)) return;
    // The player's active behaviours only (m_bIsActive, m_pPlayerController).
    const auto b = reinterpret_cast<uintptr_t>(self);
    uint8_t active = 0;
    uintptr_t controller = 0;
    float a[2];
    if (!mem::rd(b + kBehActive, active) || !active || !mem::rd(b + kBehController, controller) || !controller ||
        !mem::rd(b + kBehDeg, a) || !std::isfinite(a[0]) || !std::isfinite(a[1]))
        return;
    AcquireSRWLockShared(&g_turn_lock);
    const double yaw = g_frame_turn[0], pitch = g_frame_turn[1];
    ReleaseSRWLockShared(&g_turn_lock);
    if (yaw == 0 && pitch == 0) return;
    const float out[2] = {float(std::remainder(double(a[0]) + yaw, 360.0)),
                          float(std::fmax(-kGotgMaxPitch, std::fmin(kGotgMaxPitch, double(a[1]) + pitch)))};
    mem::guarded_store(b + kBehDeg, out, sizeof out);
}

template <int K>
void __fastcall hooked_update(void* self, void* in, void* out, void* info, float dt) {
    on_behaviour_update(self);
    g_update_orig[K](self, in, out, info, dt);
}

// Points slot kUpdateSlot of the vtable at the hook; false if it isn't the exe's code.
bool hook_vtable(uintptr_t vt, int k, void* hook) {
    auto* slot = reinterpret_cast<void**>(vt + kUpdateSlot * 8);
    void* original = nullptr;
    if (!vt || !mem::rd(reinterpret_cast<uintptr_t>(slot), original) || !mem::in_module(reinterpret_cast<uintptr_t>(original)))
        return false;
    if (!g_update_event) g_update_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_update_event) return false;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = f.QuadPart;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    g_update_orig[k] = reinterpret_cast<UpdateFn>(original);
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_hooked = true;
    log("dawn: camera behaviour update hooked (vtable %s slot %d, was %s)", mem::module_offset(vt).c_str(), kUpdateSlot,
        mem::module_offset(reinterpret_cast<uintptr_t>(original)).c_str());
    return true;
}

DWORD WINAPI frame_thread(void*) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    log("dawn: gyro frame thread %lu, %s", GetCurrentThreadId(),
        g_hooked ? "woken by each frame's camera behaviour update (20 ms without one: menus, loading)"
                 : timer ? "after each player camera update (high-resolution timer)" : "after each player camera update (Sleep)");
    LARGE_INTEGER freq, now, last_update{}, last_frame{};
    QueryPerformanceFrequency(&freq);
    const auto ms = [&](const LARGE_INTEGER& t) { return double(now.QuadPart - t.QuadPart) * 1000 / double(freq.QuadPart); };
    Pose seen{};
    for (;;) {
        if (g_hooked) {
            // The camera update's event, or every 20 ms without one.
            WaitForSingleObject(g_update_event, 20);
            g_frame();
            continue;
        }
        LARGE_INTEGER due{};
        due.QuadPart = -10000;  // 1 ms, in 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        QueryPerformanceCounter(&now);
        Pose pose;
        bool run = false;
        if (watched_pose(pose) && memcmp(&pose, &seen, sizeof pose) != 0) {
            seen = pose;
            if (ms(last_update) >= 2.5) run = true;
            last_update = now;
        }
        if (!run && ms(last_frame) < 20) continue;
        last_frame = now;
        g_frame();
    }
}

// --- vtables from the RTTI (core/rtti.hpp) --------------------------------------------------------------

using rtti::class_name;
using rtti::Image;
using rtti::primary_vtable;

Image image() { return rtti::exe_image(); }

// --- heap scan -----------------------------------------------------------------------------------------------------

constexpr int kKinds = 3;
uintptr_t g_vts[kKinds] = {};  // the vtables scanned for (0: unused)
std::atomic<bool> g_scanning{false};
std::vector<uintptr_t> g_found[kKinds];  // written by the scan thread before g_scanning goes false

// One region's matches, without C++ objects (__try).
bool scan_raw(uintptr_t base, uintptr_t end, uintptr_t* out, size_t cap, size_t& n, uint8_t* which) {
    __try {
        for (uintptr_t p = base; p < end && n < cap; p += 8) {
            const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
            for (int k = 0; k < kKinds; ++k)
                if (v == g_vts[k] && v) {
                    which[n] = uint8_t(k);
                    out[n++] = p;
                    break;
                }
        }
        return true;
    } __except (mem::guard_filter(GetExceptionInformation())) {
        return false;
    }
}

void scan_region(uintptr_t base, uintptr_t end, std::vector<uintptr_t> (&found)[kKinds]) {
    constexpr size_t kCap = 4096;
    uintptr_t out[kCap];
    uint8_t which[kCap];
    size_t n = 0;
    scan_raw(base, end, out, kCap, n, which);
    for (size_t i = 0; i < n; ++i) found[which[i]].push_back(out[i]);
}

DWORD WINAPI scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    std::vector<uintptr_t> found[kKinds];
    // This thread's own stack holds the vtables it compares against: skipped.
    ULONG_PTR stack_lo = 0, stack_hi = 0;
    GetCurrentThreadStackLimits(&stack_lo, &stack_hi);
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            if (end <= stack_lo || base >= stack_hi) scan_region(base, end, found);
        }
        a = end;
    }
    // Logged only when the counts change (in cutscenes and menus the scan repeats every 2 s with the same result).
    static size_t logged[kKinds] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
    if (found[0].size() != logged[0] || found[1].size() != logged[1] || found[2].size() != logged[2]) {
        for (int k = 0; k < kKinds; ++k) logged[k] = found[k].size();
        log("dawn: heap scan %zu / %zu / %zu objects (%llu ms)", found[0].size(), found[1].size(), found[2].size(),
            static_cast<unsigned long long>(GetTickCount64() - start));
    }
    for (int k = 0; k < kKinds; ++k) g_found[k] = std::move(found[k]);
    g_scanning = false;
    return 0;
}

// DXMD: kind 0 ZPlayerCharacter, kind 1 ZGenericCamera. GOTG: kind 0 ZCameraBehavior_AimRotation, kind 1 its _Offset
// subclass, kind 2 ZPlayerCameraSystem.
uintptr_t& g_player_vt = g_vts[0];
uintptr_t& g_camera_vt = g_vts[1];
uintptr_t g_cam_entity_vt = 0;  // GOTG: ZCameraEntity

bool read_angles(uintptr_t player, float (&a)[2]) {
    return player && mem::rd(player + kYaw, a) && std::isfinite(a[0]) && std::isfinite(a[1]) &&
           std::fabs(a[1]) < 2;
}

// How well a camera's forward agrees with the angles (cosine), -2 if unreadable.
double agreement(uintptr_t cam, const float (&a)[2]) {
    uintptr_t vt = 0;
    float f[3];
    if (!mem::rd(cam, vt) || vt != g_camera_vt || !mem::rd(cam + kForward, f)) return -2;
    const double cp = std::cos(a[1]);
    const double e[3] = {-std::sin(a[0]) * cp, -std::cos(a[0]) * cp, std::sin(a[1])};
    return f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
}

// GOTG: an active player rotation behaviour's angles (degrees).
bool behaviour_angles(uintptr_t b, float (&a)[2]) {
    uintptr_t vt = 0, controller = 0;
    uint8_t active = 0;
    return mem::rd(b, vt) && vt && (vt == g_vts[0] || vt == g_vts[1]) && mem::rd(b + kBehActive, active) && active &&
           mem::rd(b + kBehController, controller) && controller && mem::rd(b + kBehDeg, a) && std::isfinite(a[0]) &&
           std::isfinite(a[1]) && std::fabs(a[0]) < 1e5f && std::fabs(a[1]) <= 180;
}

class DawnEngine final : public Engine {
public:
    const char* name() const override { return "Dawn"; }

    bool detect(const std::wstring& exe_path) const override { return dawn_layout(exe_path); }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        if (HANDLE thread = CreateThread(nullptr, 0, &frame_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("dawn: can't start the gyro frame thread (error %lu); no gyro frame", GetLastError());
        return false;
    }

    void configure() override {
        const Image im = image();
        const auto shown = [](uintptr_t vt) { return vt ? mem::module_offset(vt) : std::string("not found"); };
        const uintptr_t player = primary_vtable(im, ".?AVZPlayerCharacter@@");
        const uintptr_t camera = primary_vtable(im, ".?AVZGenericCamera@@");
        if (player && camera) {
            route_ = Route::Character;
            g_vts[0] = player;
            g_vts[1] = camera;
            log("dawn: player character route: ZPlayerCharacter vtable %s, ZGenericCamera vtable %s",
                shown(player).c_str(), shown(camera).c_str());
        } else {
            g_vts[0] = primary_vtable(im, ".?AVZCameraBehavior_AimRotation@@");
            g_vts[1] = primary_vtable(im, ".?AVZCameraBehavior_AimRotation_Offset@@");
            g_vts[2] = primary_vtable(im, ".?AVZPlayerCameraSystem@@");
            g_cam_entity_vt = primary_vtable(im, ".?AVZCameraEntity@@");
            route_ = g_vts[0] ? Route::Behaviour : Route::None;
            if (g_vts[0] && !hook_vtable(g_vts[0], 0, reinterpret_cast<void*>(&hooked_update<0>)))
                log("dawn: ZCameraBehavior_AimRotation's update not hooked: turns written from Gyro4All's thread");
            if (g_vts[1]) hook_vtable(g_vts[1], 1, reinterpret_cast<void*>(&hooked_update<1>));
            log("dawn: %s: ZCameraBehavior_AimRotation vtable %s, _Offset %s, ZPlayerCameraSystem %s, ZCameraEntity %s "
                "(ZPlayerCharacter %s, ZGenericCamera %s)",
                g_vts[0] ? "camera behaviour route" : "no known camera classes (gyro as right stick)",
                shown(g_vts[0]).c_str(), shown(g_vts[1]).c_str(), shown(g_vts[2]).c_str(), shown(g_cam_entity_vt).c_str(),
                shown(player).c_str(), shown(camera).c_str());
        }
        load_zoom_reference();
    }

    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    EngineFrame read_frame() override { return route_ == Route::Behaviour ? read_behaviour() : read_character(); }

    // Degrees from GyroLib (pitch up, yaw right), added to the view's angles.
    bool apply(double pitch, double yaw) override { return route_ == Route::Behaviour ? apply_behaviour(pitch, yaw) : apply_character(pitch, yaw); }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (route_ == Route::Behaviour) {
            if (!g_vts[0]) return "view=default (gyro as right stick)";
            return live_ ? "view=default (" + std::to_string(active_.size()) + " aim behaviour(s))"
                         : "view=none (no active aim behaviour)";
        }
        if (!g_player_vt || !g_camera_vt) return "view=default (gyro as right stick)";
        return live_ ? "view=default (player camera)" : player_ ? "view=none (another camera)" : "view=none (no player)";
    }

private:
    // --- DXMD ---
    EngineFrame read_character() {
        EngineFrame f;
        if (!g_player_vt || !g_camera_vt) {
            f.in_game = true;
            f.no_camera = true;
            f.view_id = 1;
            f.target = "no RTTI (gyro as right stick)";
            return f;
        }
        if (!g_scanning) adopt_scan();
        float a[2];
        uintptr_t vt = 0;
        const bool player_ok = player_ && mem::rd(player_, vt) && vt == g_player_vt && read_angles(player_, a);
        if (!player_ok) {
            player_ = camera_ = 0;
            rescan();
        } else if (!camera_ || agreement(camera_, a) < kMatchCos) {
            // A short mismatch (a fast turn the camera hasn't caught up with, a brief blend) keeps the camera: only one
            // lasting kMismatchMs is another camera. An unreadable camera is dropped at once.
            const ULONGLONG now = GetTickCount64();
            if (camera_ && agreement(camera_, a) > -2) {
                if (!mismatch_since_) mismatch_since_ = now;
                if (now - mismatch_since_ >= kMismatchMs) {
                    ++long_mismatches_;
                    mismatch_since_ = 0;
                    camera_ = pick_camera(a);
                } else if (now - mismatch_since_ < 2 && short_logs_ < 20) {
                    ++short_logs_;
                    log("dawn: the camera's view and the angles differ (%.1f degrees): kept for up to %llu ms",
                        std::acos(std::fmax(-1.0, std::fmin(1.0, agreement(camera_, a)))) * 180 / kPi,
                        static_cast<unsigned long long>(kMismatchMs));
                }
            } else {
                mismatch_since_ = 0;
                camera_ = pick_camera(a);
            }
        } else {
            mismatch_since_ = 0;
        }
        watch(0, camera_ ? camera_ + 0x68 : 0, 24);
        const bool live = player_ok && camera_;
        if (live != live_)
            log("dawn: %s", live ? "player camera follows the angles: gyro on"
                                 : player_ok ? "the view is another camera (cutscene, cover, takedown, menu): gyro waits"
                                             : "no player (front end, loading)");
        live_ = live;
        f.in_game = player_ok;
        f.view_id = live ? 1 : 0;
        f.target = live ? "player camera" : player_ok ? "another camera" : "no player";
        if (live) f.zoom_fov = zoom_fov(camera_ + kFov);
        return f;
    }

    bool apply_character(double pitch, double yaw) {
        float a[2];
        if (!live_ || !read_angles(player_, a)) return false;
        const double p = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, double(a[1]) + kPitchSense * pitch * kPi / 180));
        const float out[2] = {float(std::remainder(double(a[0]) + kYawSense * yaw * kPi / 180, 2 * kPi)), float(p)};
        return mem::guarded_store(player_ + kYaw, out, sizeof out);
    }

    // --- GOTG ---
    EngineFrame read_behaviour() {
        EngineFrame f;
        if (!g_vts[0]) {
            f.in_game = true;
            f.no_camera = true;
            f.view_id = 1;
            f.target = "no RTTI (gyro as right stick)";
            return f;
        }
        if (!g_scanning) adopt_scan();
        // The player's camera (the ZPlayerCameraSystem's ZCameraEntity) and its modes' active player aim behaviours,
        // read every frame: the behaviours are made and freed with their modes (aiming, sprint, combat).
        float a[2];
        bool free_cam = false, paused = false;
        if (!camera_valid(camera_)) camera_ = 0;
        if (system_ && !camera_valid(system_camera(system_))) system_ = 0;
        if (!camera_) {
            for (uintptr_t s : systems_)
                if (const uintptr_t c = system_camera(s); camera_valid(c)) {
                    system_ = s;
                    camera_ = c;
                    log("dawn: player camera at %s (%s)", mem::module_offset(c).c_str(), class_name(c).c_str());
                    break;
                }
            if (!camera_) rescan();
        }
        active_.clear();
        if (camera_) collect_active(camera_);
        // The player's camera is the one hosting the active aim behaviour's mode (ZCameraBehavior::m_mode +0x28,
        // ZCameraMode::m_pCamera +0x28). New behaviours of known modes come through the tree walk; with none active,
        // the behaviours found by the last scan are checked directly, and with none of those either, a new scan.
        if (active_.empty()) {
            for (uintptr_t b : scanned_)
                if (active_.size() < kMaxActive && behaviour_angles(b, a)) active_.push_back(b);
            if (!active_.empty()) {
                const uintptr_t c = behaviour_camera(active_[0]);
                if (camera_valid(c) && c != camera_) {
                    camera_ = c;
                    system_ = 0;
                    for (uintptr_t s : systems_)
                        if (system_camera(s) == c) system_ = s;
                    log("dawn: player camera at %s (%s), from the active aim behaviour's mode%s",
                        mem::module_offset(c).c_str(), class_name(c).c_str(), system_ ? "" : " (no camera system)");
                }
                std::vector<uintptr_t> direct = active_;
                active_.clear();
                if (camera_) collect_active(camera_);
                if (active_.empty()) active_ = direct;
            } else if (GetTickCount64() - last_active_ >= 300) {
                rescan();
            }
        }
        if (!active_.empty()) last_active_ = GetTickCount64();
        if (camera_) {
            uint8_t b = 0;
            free_cam = system_ && mem::rd(system_ + kSysFreeCam, b) && b;
            paused = mem::rd(camera_ + kCamPaused, b) && b;
        }
        watch(0, active_.empty() ? 0 : active_[0] + kBehShot, kBehShotSize);
        watch(1, camera_ ? camera_ + kRcWorld : 0, 48);
        // Hooked: the aim behaviours must be the ones the game updates. In some states (aiming, combat) the view is
        // driven by behaviours of another class and these only follow it: a turn left for them would wait, then land
        // all at once when they update again.
        bool updating = true;
        if (g_hooked && !active_.empty()) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const long long last = g_hook_last.load(std::memory_order_relaxed);
            updating = last && (now.QuadPart - last) * 1000 < g_qpc_freq * 50;
        }
        const bool live = !active_.empty() && !free_cam && !paused && updating;
        // Two lines per dialogue, cutscene or menu: the first 100 only.
        if (live != live_ && state_logs_ < 100) {
            ++state_logs_;
            log("dawn: %s%s",
                live       ? "player aim rotation active: gyro on"
                : free_cam ? "free camera (photo mode): gyro waits"
                : paused   ? "camera paused: gyro waits"
                : !active_.empty() ? "the aim behaviours aren't updated (another camera drives the view): gyro waits"
                                   : "no active player aim rotation (cutscene, menu, loading): gyro waits",
                state_logs_ == 100 ? " (no more gyro on/waits lines)" : "");
        }
        live_ = live;
        g_turn_live.store(live, std::memory_order_relaxed);
        if (!live && g_hooked) {
            AcquireSRWLockExclusive(&g_turn_lock);
            g_pending[0] = g_pending[1] = g_frame_turn[0] = g_frame_turn[1] = 0;
            ReleaseSRWLockExclusive(&g_turn_lock);
        }
        f.in_game = camera_ != 0;
        f.paused = paused;
        f.view_id = live ? 1 : 0;
        f.target = live ? "aim rotation" : "no aim rotation";
        if (live && camera_) f.zoom_fov = zoom_fov(camera_ + kRcFov);
        return f;
    }

    bool apply_behaviour(double pitch, double yaw) {
        if (!live_) return false;
        if (g_hooked) {
            // Left for the next camera update, which adds it to its own behaviour's angles (on_behaviour_update).
            AcquireSRWLockExclusive(&g_turn_lock);
            g_pending[0] += kGotgYawSense * yaw;
            g_pending[1] += kGotgPitchSense * pitch;
            ReleaseSRWLockExclusive(&g_turn_lock);
            return true;
        }
        bool any = false;
        for (uintptr_t b : active_) {
            float a[2];
            if (!behaviour_angles(b, a)) continue;
            const double p =
                std::fmax(-kGotgMaxPitch, std::fmin(kGotgMaxPitch, double(a[1]) + kGotgPitchSense * pitch));
            const float out[2] = {float(std::remainder(double(a[0]) + kGotgYawSense * yaw, 360.0)), float(p)};
            if (mem::guarded_store(b + kBehDeg, out, sizeof out)) any = true;
        }
        return any;
    }

    // --- shared ---
    static void watch(int i, uintptr_t at, size_t n) {
        g_watch_size[i].store(n, std::memory_order_relaxed);
        g_watch[i].store(at, std::memory_order_relaxed);
    }

    // GOTG: a ZPlayerCameraSystem's camera, if it is a live ZCameraEntity.
    static uintptr_t system_camera(uintptr_t s) {
        uintptr_t vt = 0, c = 0;
        return mem::rd(s, vt) && vt == g_vts[2] && mem::rd(s + kSysCamera, c) ? c : 0;
    }
    static bool camera_valid(uintptr_t c) {
        uintptr_t vt = 0;
        float fov = 0;
        return c && mem::rd(c, vt) && vt == g_cam_entity_vt && mem::rd(c + kRcFov, fov) && fov::is_valid(fov);
    }

    // GOTG: the camera hosting a behaviour's mode.
    static uintptr_t behaviour_camera(uintptr_t b) {
        uintptr_t mode = 0, cam = 0;
        return mem::rd(b + kBehMode, mode) && mode && mem::rd(mode + kModeCamera, cam) ? cam : 0;
    }

    // GOTG: the active player aim behaviours of the camera's modes (its mode stack and overrides).
    void collect_active(uintptr_t camera) {
        const auto add_mode = [&](uintptr_t mode) {
            struct { uintptr_t start; uint32_t count; } arr{};
            if (!mode || !mem::rd(mode + kModeBehaviours, arr) || !arr.start || arr.count > 64) return;
            for (uint32_t i = 0; i < arr.count && active_.size() < kMaxActive; ++i) {
                uintptr_t b = 0;
                float a[2];
                if (mem::rd(arr.start + i * 16, b) && behaviour_angles(b, a) &&
                    std::find(active_.begin(), active_.end(), b) == active_.end())
                    active_.push_back(b);
            }
        };
        struct { uintptr_t start; uint32_t count; } stack{}, overrides{};
        if (mem::rd(camera + kCamModeStack, stack) && stack.start && stack.count <= 32)
            for (uint32_t i = 0; i < stack.count; ++i) {
                uintptr_t mode = 0;
                if (mem::rd(stack.start + i * 24, mode)) add_mode(mode);
            }
        if (mem::rd(camera + kCamModeOverrides, overrides) && overrides.start && overrides.count <= 32)
            for (uint32_t i = 0; i < overrides.count; ++i) {
                uintptr_t mode = 0;
                if (mem::rd(overrides.start + i * 16, mode)) add_mode(mode);
            }
    }

    void rescan() {
        const ULONGLONG now = GetTickCount64();
        if (g_scanning || now < next_scan_) return;
        next_scan_ = now + kRescanMs;
        start_scan();
    }

    void start_scan() {
        if (g_scanning) return;
        scan_pending_ = true;
        g_scanning = true;
        if (HANDLE t = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr)) CloseHandle(t);
        else g_scanning = false;
    }

    void adopt_scan() {
        if (!scan_pending_) return;
        scan_pending_ = false;
        if (route_ == Route::Behaviour) {
            systems_ = g_found[2];
            scanned_ = g_found[0];
            scanned_.insert(scanned_.end(), g_found[1].begin(), g_found[1].end());
            return;
        }
        cameras_ = g_found[1];
        const auto& players = g_found[0];
        player_ = players.size() == 1 ? players[0] : 0;
        if (!player_ && players.size() > 1 && !warned_many_) {
            warned_many_ = true;
            log("dawn: %zu player characters: not turning any", players.size());
        }
        if (player_) log("dawn: player at %s", mem::module_offset(player_).c_str());
    }

    // DXMD: the camera whose forward matches the angles best (and well enough); a rescan when none does.
    uintptr_t pick_camera(const float (&a)[2]) {
        uintptr_t best = 0;
        double best_cos = kMatchCos;
        for (uintptr_t c : cameras_)
            if (const double d = agreement(c, a); d >= best_cos) best = c, best_cos = d;
        if (best && best != logged_camera_) {
            logged_camera_ = best;
            log("dawn: player camera at %s", mem::module_offset(best).c_str());
        }
        if (!best) {
            // A camera made since the last scan (a new level), or a scripted one is in view: look again now and then.
            const ULONGLONG now = GetTickCount64();
            if (now >= next_camera_scan_) {
                next_camera_scan_ = now + 5000;
                start_scan();
            }
        }
        return best;
    }

    // [Gyro4All] ZoomReferenceFOV= (horizontal degrees); 0 learns the unzoomed FOV: the widest one seen.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_fov_)
            log("dawn: zoom reference FOV %s", fov > 0 ? (std::to_string(int(fov)) + " degrees horizontal").c_str()
                                                       : "learnt in game (the widest FOV seen)");
        zoom_reference_fov_ = fov;
    }

    // The vertical FOV in degrees at `at`.
    std::optional<ZoomFov> zoom_fov(uintptr_t at) {
        float fov = 0;
        if (!mem::rd(at, fov) || !fov::is_valid(fov)) return std::nullopt;
        if (fov > widest_) {
            widest_ = fov;
            log("dawn: widest FOV seen %.1f degrees vertical", widest_);
        }
        ZoomFov z;
        z.current_vertical = fov;
        z.reference_vertical = zoom_reference_fov_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_fov_, fov::game_window_aspect())
                                   : widest_;
        if (!fov::is_valid(z.reference_vertical)) return std::nullopt;
        if (std::fabs(fov - logged_fov_) > 1 && fov_logs_ < 50) {
            ++fov_logs_;
            logged_fov_ = fov;
            log("dawn: FOV %.1f degrees vertical (unzoomed %.1f)", z.current_vertical, z.reference_vertical);
        }
        return z;
    }

    Route route_ = Route::None;
    uintptr_t player_ = 0, camera_ = 0, logged_camera_ = 0;
    std::vector<uintptr_t> cameras_;
    std::vector<uintptr_t> systems_, active_;  // GOTG
    uintptr_t system_ = 0;
    int state_logs_ = 0;
    std::vector<uintptr_t> scanned_;
    ULONGLONG last_active_ = 0;
    bool live_ = false, scan_pending_ = false, warned_many_ = false;
    ULONGLONG next_scan_ = 0, next_camera_scan_ = 0;
    double zoom_reference_fov_ = -1;
    double widest_ = 0, logged_fov_ = 0;
    int fov_logs_ = 0;
    ULONGLONG mismatch_since_ = 0;
    int long_mismatches_ = 0, short_logs_ = 0;
};

}  // namespace

Engine& engine() {
    static DawnEngine e;
    return e;
}

}  // namespace gyro4all::dawn
