// PlatinumGames' engine: NieR:Automata (games/NieRAutomata/MODDING_PLAN.md; MODLOG 2026-11-15, found in the July 2021
// Steam exe with camtrace). Native x64, D3D11, data in data\*.cpk. The exe's code is plain on disk (SteamStub only
// wraps its .bind section). The controller reaches the game through the core's XInput translation (XInput=1).
//
// The camera: a C++ object (a static one in that build, vtable exe+0xca7988 at exe+0x1020870). Its orbit update adds an
// angular velocity (+0x4ac, +0x4b0) to its pitch and yaw (+0x3f0 up, +0x3f4; float radians), builds the goal eye and
// look-at from them (+0x140, +0x150), and the view eases toward those goals every frame (about 1 s). The angles are the
// source: the view follows a write there and the game keeps it. Everything after them (the eye, look-at and the angles
// derived from them at +0x40/+0x44) is rewritten each frame.
//
// Two update functions do that (both start by testing +0x4c0, then add the velocity to the angles): they are found by
// that code and the unwind table, and hooked. The hook adds the gyro's turn to the angles right before the game's update
// runs, so the turn goes through the game's own camera logic. While neither has run for 100 ms (menus, pause, loading,
// cutscenes, other camera modes), there is no gameplay view and the gyro waits. Zoom compensation reads the view's
// vertical FOV (+0x138) against the game's default (50 degrees) or ZoomReferenceFOV=.
#include "platinum.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace gyro4all::platinum {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

constexpr double kPi = 3.14159265358979323846;
constexpr uintptr_t kPitch = 0x3f0, kYaw = 0x3f4;  // in the camera object, float radians (pitch up positive)
// The gyro keeps the pitch within this (radians, 74 degrees); the game's own limit is unknown.
constexpr double kMaxPitch = 1.3;
// Yaw sense: +1 when the game's yaw grows turning right.
constexpr double kYawSense = -1;
constexpr ULONGLONG kIdleMs = 100;
// The view's vertical FOV (float radians), eased toward the goal at +0x188 like the eye and look-at; the goal is the orbit
// update's own FOV blended with a scripted one (+0x358). The render camera ([camera+0x60]) takes it as its vertical FOV
// (+0x274; cot(fov/2) is the projection's y scale, divided by the aspect at +0x270 for x). 50 degrees in the open world.
constexpr uintptr_t kFov = 0x138;
constexpr double kDefaultFov = 50;  // vertical degrees, the unzoomed reference without ZoomReferenceFOV=

// movss xmm0,[rcx+3F0h]; addss xmm0,[rcx+4ACh]: both orbit updates add the velocity to the pitch right at their start.
constexpr uint8_t kAngleCode[] = {0xF3, 0x0F, 0x10, 0x81, 0xF0, 0x03, 0x00, 0x00,
                                  0xF3, 0x0F, 0x58, 0x81, 0xAC, 0x04, 0x00, 0x00};

// The orbit updates' arguments (July 2021 exe): (camera, ?, float, float, stack float, stack int) at exe+0x727260 and
// (camera, ?, ?, float, stack ?, stack float, stack float) at exe+0x727910. Stack arguments are passed on as raw 8-byte
// slots.
using UpdateA_t = void (*)(uintptr_t, uintptr_t, float, float, uint64_t, uint64_t);
using UpdateB_t = void (*)(uintptr_t, uintptr_t, uintptr_t, float, uint64_t, uint64_t, uint64_t);
UpdateA_t g_update_a = nullptr;
UpdateB_t g_update_b = nullptr;
int g_hooks = 0;

std::mutex g_lock;
double g_pitch = 0, g_yaw = 0;  // radians, in the game's senses, not applied yet
std::atomic<uintptr_t> g_camera{0};
std::atomic<ULONGLONG> g_last_run{0};

void turn(uintptr_t cam) {
    if (g_camera.exchange(cam) != cam) log("platinum: orbit camera %s", mem::module_offset(cam).c_str());
    g_last_run = GetTickCount64();
    double pitch, yaw;
    {
        std::lock_guard<std::mutex> l(g_lock);
        pitch = g_pitch;
        yaw = g_yaw;
        g_pitch = g_yaw = 0;
    }
    if (pitch == 0 && yaw == 0) return;
    float a[2];
    if (!mem::rd(cam + kPitch, a) || !std::isfinite(a[0]) || !std::isfinite(a[1])) return;
    const double p = std::fmax(-kMaxPitch, std::fmin(kMaxPitch, double(a[0]) + pitch));
    const float out[2] = {float(p), float(std::remainder(double(a[1]) + yaw, 2 * kPi))};
    mem::guarded_store(cam + kPitch, out, sizeof out);
}

void hk_update_a(uintptr_t cam, uintptr_t a2, float a3, float a4, uint64_t a5, uint64_t a6) {
    turn(cam);
    g_update_a(cam, a2, a3, a4, a5, a6);
}

void hk_update_b(uintptr_t cam, uintptr_t a2, uintptr_t a3, float a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    turn(cam);
    g_update_b(cam, a2, a3, a4, a5, a6, a7);
}

// The start of the function holding pc, through the unwind table (chained entries followed to the primary one).
uintptr_t function_start(uintptr_t pc) {
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(pc, &image, nullptr);
    for (int i = 0; f && i < 32; ++i) {
        const auto* info = reinterpret_cast<const uint8_t*>(image + f->UnwindData);
        if (!(info[0] >> 3 & UNW_FLAG_CHAININFO)) return uintptr_t(image + f->BeginAddress);
        const uint8_t codes = info[2];
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + 2 * ((codes + 1) & ~1));
    }
    return 0;
}

// The orbit updates: every match of kAngleCode in the exe's code, each function's prologue checked against the two
// known ones (their register/stack layouts are what the hooks pass on).
void install() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    // mov rax,rsp; push rbp; push rbx; push rsi; push rdi; push r15 (A) / push r14 (B); lea rbp,[rax-...]
    constexpr uint8_t kPrologA[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x08, 0xFF};
    constexpr uint8_t kPrologB[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xA8, 0x18, 0xFF};
    int matches = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(base + s->VirtualAddress);
        const size_t n = s->Misc.VirtualSize;
        for (size_t at = 0; at + sizeof kAngleCode <= n; ++at) {
            if (p[at] != kAngleCode[0] || memcmp(p + at, kAngleCode, sizeof kAngleCode) != 0) continue;
            ++matches;
            const uintptr_t fn = function_start(uintptr_t(p + at));
            if (!fn) continue;
            const auto* code = reinterpret_cast<const uint8_t*>(fn);
            const bool a = memcmp(code, kPrologA, sizeof kPrologA) == 0;
            const bool b = !a && memcmp(code, kPrologB, sizeof kPrologB) == 0;
            void** original = a && !g_update_a ? reinterpret_cast<void**>(&g_update_a)
                              : b && !g_update_b ? reinterpret_cast<void**>(&g_update_b)
                                                 : nullptr;
            if (!original) {
                log("platinum: orbit code at %s in a function at %s with an unknown layout: not hooked",
                    mem::module_offset(uintptr_t(p + at)).c_str(), mem::module_offset(fn).c_str());
                continue;
            }
            void* hook = a ? reinterpret_cast<void*>(&hk_update_a) : reinterpret_cast<void*>(&hk_update_b);
            if (hooks::hook_function(reinterpret_cast<void*>(fn), hook, original)) {
                ++g_hooks;
                log("platinum: orbit camera update %c hooked at %s", a ? 'A' : 'B', mem::module_offset(fn).c_str());
            } else {
                *original = nullptr;
                log("platinum: orbit camera update at %s could not be hooked", mem::module_offset(fn).c_str());
            }
        }
    }
    if (!g_hooks)
        log("platinum: no orbit camera update hooked (its code matched %d times): gyro as right stick", matches);
}

uintptr_t live_camera() {
    const uintptr_t cam = g_camera.load();
    return cam && GetTickCount64() - g_last_run.load() <= kIdleMs ? cam : 0;
}

class PlatinumEngine final : public Engine {
public:
    const char* name() const override { return "Platinum"; }

    // NieR:Automata: <root>\NieRAutomata*.exe with <root>\data\data000.cpk.
    bool detect(const std::wstring& exe_path) const override {
        const size_t slash = exe_path.find_last_of(L"\\/");
        const std::wstring dir = exe_path.substr(0, slash), exe = exe_path.substr(slash + 1);
        return _wcsnicmp(exe.c_str(), L"NieRAutomata", 12) == 0 && exists(dir + L"\\data\\data000.cpk");
    }

    void configure() override {
        install();
        load_zoom_reference();
    }

    void settings_changed() override { load_zoom_reference(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, g_hooks > 0}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        f.in_game = true;
        if (!g_hooks) {
            f.no_camera = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        const uintptr_t cam = live_camera();
        if ((cam != 0) != live_) log("platinum: orbit camera %s", cam ? "running" : "idle (menus, pause, cutscene or another camera)");
        live_ = cam != 0;
        f.view_id = live_ ? 1 : 0;
        f.target = live_ ? "orbit camera" : "no orbit camera";
        if (live_) f.zoom_fov = zoom_fov(cam);
        return f;
    }

    // Degrees from GyroLib (pitch up, yaw right), handed to the next orbit update.
    bool apply(double pitch, double yaw) override {
        if (!live_camera()) return false;
        std::lock_guard<std::mutex> l(g_lock);
        g_pitch += pitch * kPi / 180;
        g_yaw += kYawSense * yaw * kPi / 180;
        return true;
    }

    bool sees_menus() const override { return true; }

    std::string status_scene() const override {
        if (!g_hooks) return "view=default (gyro as right stick)";
        return live_ ? "view=default (orbit camera)" : "view=none (no orbit camera)";
    }

private:
    // [Gyro4All] ZoomReferenceFOV= (horizontal degrees, profile or Gyro4All.ini); 0 uses the game's default. At start and
    // on save.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_fov_)
            log("platinum: zoom reference FOV %.1f degrees%s", fov > 0 ? fov : kDefaultFov,
                fov > 0 ? " horizontal ([Gyro4All] ZoomReferenceFOV)" : " vertical (the game's default)");
        zoom_reference_fov_ = fov;
    }

    // The current and unzoomed FOV, both vertical degrees, for GyroLib's zoom compensation. Changes are logged (a few).
    std::optional<ZoomFov> zoom_fov(uintptr_t cam) {
        float radians = 0;
        if (!mem::rd(cam + kFov, radians)) return std::nullopt;
        ZoomFov z;
        z.current_vertical = radians * 180 / kPi;
        z.reference_vertical = zoom_reference_fov_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_fov_, fov::game_window_aspect())
                                   : kDefaultFov;
        if (!fov::is_valid(z.current_vertical) || !fov::is_valid(z.reference_vertical)) return std::nullopt;
        if (std::fabs(z.current_vertical - logged_fov_) > 1 && fov_logs_ < 200) {
            ++fov_logs_;
            log("platinum: FOV %.1f degrees vertical (unzoomed %.1f)", z.current_vertical, z.reference_vertical);
            logged_fov_ = z.current_vertical;
        }
        return z;
    }

    bool live_ = false;
    double zoom_reference_fov_ = -1;  // [Gyro4All] ZoomReferenceFOV=, horizontal degrees; 0 = the game's default, -1 unread
    double logged_fov_ = 0;
    int fov_logs_ = 0;
};

}  // namespace

Engine& engine() {
    static PlatinumEngine e;
    return e;
}

}  // namespace gyro4all::platinum
