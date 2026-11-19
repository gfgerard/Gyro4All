// Toylogic's engine (namespaces tp::, snow::): NieR Replicant ver.1.22474487139 (MODLOG 2026-11-19, found in the Steam
// exe with camtrace). Native x64, D3D11, data in data\*.arc. Not PlatinumGames' engine (NieR:Automata). No PlayStation
// support: the game reads controllers through DirectInput and makes an XInput one only of those Microsoft's WMI
// IsXInputDevice check accepts; the core's XInput translation (auto mode, its WMI answer) shows the DualSense as one.
//
// The camera: the player camera's yaw and pitch are static floats (radians), each the value of a lerp_parameter_rad<float>
// (+0x20). The float right after it (+0x28) is the output the view follows: the value plus the game's own adjustments
// (it can differ by several degrees, e.g. pitch on slopes). The game keeps a write to the value and the view follows it
// in the same frame: the values are the source; a write to the output is undone. Yaw is atan2(forward.x, forward.z) of the view, pitch + = down. The cameras
// (CBehindCamera, CLookAtEnemyCamera...) copy them into their own fields (+0x410 yaw, +0x438 pitch, clamped to
// +-1.4765 rad by CBehindCamera's update).
//
// Found by the code that copies both into a camera (exe+0x34a50b in that build): movss xmm0,[yaw]; mov rcx,rbx;
// movss [rbx+410h],xmm0; movss xmm1,[pitch]; movss [rbx+438h],xmm1.
//
// The gate: the view's forward vector (the render camera, a static copy at a build-specific address) must point where
// the angles say, within a few degrees. Cutscenes, scripted and fixed cameras don't use those angles, so the gyro waits
// there. Without that copy (another build), the gyro always turns (no gate).
#include "toylogic.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::toylogic {

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

constexpr double kPi = 3.14159265358979323846;
// The gyro keeps the pitch within this (radians, 80 degrees); CBehindCamera clamps its copy at 1.4765.
constexpr double kMaxPitch = 1.40;
// Yaw sense: +1 when the game's yaw grows turning right; pitch sense: +1 when the game's pitch grows looking up.
constexpr double kYawSense = -1;  // checked in game: the yaw grows turning left
constexpr double kPitchSense = -1;  // checked in game
// The gate: the largest angle (degrees) between the view's forward and the angles' direction in gameplay.
constexpr double kMatchDegrees = 4;
// The parameter's output, right after the value (the view follows it).
constexpr uintptr_t kOutput = 8;
// Time without a match before the gate closes (ms). Gyro4All's frames don't follow the game's here: the game calls
// PeekMessageW in a loop between its frames (about 2000 gyro frames per second at 60 fps).
constexpr ULONGLONG kOffMs = 150;
// Window over which the pitch's output is checked against the gyro's pushes (ms).
constexpr ULONGLONG kFollowMs = 50;

// The render camera's forward (3 floats, w = 0) in the June 2021 Steam build: exe+0x17b9ae0, checked by the exe's
// time stamp and image size.
constexpr DWORD kKnownTimeStamp = 1622621184;
constexpr DWORD kKnownImageSize = 0x5339000;
constexpr uintptr_t kKnownForward = 0x17b9ae0;

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

class ToylogicEngine final : public Engine {
public:
    const char* name() const override { return "Toylogic"; }

    // NieR Replicant: <root>\NieR Replicant*.exe with <root>\data\info.arc.
    bool detect(const std::wstring& exe_path) const override {
        const size_t slash = exe_path.find_last_of(L"\\/");
        const std::wstring dir = exe_path.substr(0, slash), exe = exe_path.substr(slash + 1);
        return _wcsnicmp(exe.c_str(), L"NieR Replicant", 14) == 0 && exists(dir + L"\\data\\info.arc");
    }

    void configure() override { find_angles(); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, false}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        f.in_game = true;
        if (!yaw_) {
            f.no_camera = true;
            f.view_id = 1;
            f.target = "no camera angles (gyro as right stick)";
            return f;
        }
        // On at the first match, off only after kOffMs without one (the view lags a frame while turning).
        const ULONGLONG now = GetTickCount64();
        if (gameplay()) last_match_ = now;
        const bool live = last_match_ && now - last_match_ <= kOffMs;
        if (live != live_) log("toylogic: player camera %s", live ? "in control" : "not in control (cutscene, menu or another camera)");
        live_ = live;
        if (!live_) {
            pushed_pitch_ = 0;
            window_start_ = 0;
            blocked_pitch_ = 0;
        }
        f.view_id = live_ ? 1 : 0;
        f.target = live_ ? "player camera" : "another camera";
        return f;
    }

    // The game limits the pitch's output, not the value: past the limit the value would keep growing while the view
    // stays put (and the stick would then have to bring it all back). When the output didn't follow most of the last
    // pitch push, the gyro stops pushing further that way.
    void limit_turn(double& pitch, double& /*yaw*/) override {
        float out = 0;
        if (!live_ || !mem::rd(pitch_ + kOutput, out)) return;
        const ULONGLONG now = GetTickCount64();
        if (!window_start_) {
            window_start_ = now;
            window_out_ = out;
            pushed_pitch_ = 0;
        } else if (now - window_start_ >= kFollowMs) {
            // Pushes of 0.01 rad or more tell whether the view follows (1 = it took all of them).
            if (std::fabs(pushed_pitch_) >= 0.01) {
                const double followed = (double(out) - window_out_) / pushed_pitch_;
                blocked_pitch_ = followed < 0.25 ? (pushed_pitch_ > 0 ? 1 : -1) : 0;
            }
            window_start_ = now;
            window_out_ = out;
            pushed_pitch_ = 0;
        }
        const double dp = kPitchSense * pitch * kPi / 180;  // this frame's push, the game's sense
        if (blocked_pitch_ != 0 && dp * blocked_pitch_ > 0) pitch = 0;
        else if (dp * blocked_pitch_ < 0) blocked_pitch_ = 0;  // back away from the limit
    }

    // Degrees from GyroLib (pitch up, yaw right), added to the game's angles.
    bool apply(double pitch, double yaw) override {
        if (!live_) return false;
        float a[2];
        if (!mem::rd(yaw_, a[0]) || !mem::rd(pitch_, a[1]) || !std::isfinite(a[0]) || !std::isfinite(a[1])) return false;
        const double dp = kPitchSense * pitch * kPi / 180;
        const float y = float(std::remainder(double(a[0]) + kYawSense * yaw * kPi / 180, 2 * kPi));
        const float p = float(std::fmax(-kMaxPitch, std::fmin(kMaxPitch, double(a[1]) + dp)));
        if (!mem::guarded_store(yaw_, &y, sizeof y) || !mem::guarded_store(pitch_, &p, sizeof p)) return false;
        pushed_pitch_ += dp;
        return true;
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        float y = 0, p = 0;
        if (!live_ || !mem::rd(yaw_, y) || !mem::rd(pitch_, p)) return false;
        pitch = kPitchSense * p * 180 / kPi;
        yaw = kYawSense * y * 180 / kPi;
        camera = yaw_;
        return true;
    }

    bool sees_menus() const override { return forward_ != 0; }

    std::string status_scene() const override {
        if (!yaw_) return "view=default (gyro as right stick)";
        if (!forward_) return "view=default (player camera, no gate)";
        return live_ ? "view=default (player camera)" : "view=none (another camera)";
    }

private:
    // The angles' addresses from the code copying them into a camera; the render forward for the gate.
    void find_angles() {
        const char* kCopy = "F3 0F 10 05 ?? ?? ?? ?? 48 8B CB F3 0F 11 83 10 04 00 00 F3 0F 10 0D ?? ?? ?? ?? F3 0F 11 8B 38 04 00 00";
        std::vector<uint8_t> bytes, mask;
        mem::parse_pattern(kCopy, bytes, mask);
        const auto hits = mem::find_pattern(mem::code_sections(exe_base()), bytes.data(), mask.data(), bytes.size());
        if (hits.empty()) {
            log("toylogic: the camera angles' code was not found: gyro as right stick");
            return;
        }
        if (hits.size() > 1) log("toylogic: the camera angles' code matched %zu times, using the first", hits.size());
        const uintptr_t at = hits[0];
        int32_t d_yaw = 0, d_pitch = 0;
        mem::rd(at + 4, d_yaw);
        mem::rd(at + 23, d_pitch);
        yaw_ = at + 8 + d_yaw;
        pitch_ = at + 27 + d_pitch;
        log("toylogic: camera yaw %s, pitch %s (code at %s)", mem::module_offset(yaw_).c_str(),
            mem::module_offset(pitch_).c_str(), mem::module_offset(at).c_str());

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe_base());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe_base() + dos->e_lfanew);
        if (nt->FileHeader.TimeDateStamp == kKnownTimeStamp && nt->OptionalHeader.SizeOfImage == kKnownImageSize) {
            forward_ = exe_base() + kKnownForward;
            log("toylogic: view forward %s (gate on)", mem::module_offset(forward_).c_str());
        } else {
            log("toylogic: unknown build (time stamp %lu): no view forward, the gyro turns in every camera",
                nt->FileHeader.TimeDateStamp);
        }
    }

    // The player camera is in control: the view looks where the angles say.
    bool gameplay() const {
        if (!forward_) return true;
        // The view follows each parameter's output (+8: the value after the game's own adjustments, e.g. terrain pitch),
        // not the value the stick and the gyro turn, which can differ from it by several degrees.
        float y = 0, p = 0, v[3];
        if (!mem::rd(yaw_ + kOutput, y) || !mem::rd(pitch_ + kOutput, p) || !mem::rd(forward_, v)) return false;
        const double len = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
        if (!std::isfinite(len) || len < 0.9 || len > 1.1) return false;
        // The view may still show the previous frame's angles.
        const auto matches = [&](double yy, double pp) {
            const double cp = std::cos(pp);
            const double e[3] = {std::sin(yy) * cp, -std::sin(pp), std::cos(yy) * cp};
            return (e[0] * v[0] + e[1] * v[1] + e[2] * v[2]) / len >= std::cos(kMatchDegrees * kPi / 180);
        };
        const bool ok = matches(y, p) || (have_last_ && matches(last_y_, last_p_));
        last_y_ = y;
        last_p_ = p;
        have_last_ = true;
        return ok;
    }

    uintptr_t yaw_ = 0, pitch_ = 0, forward_ = 0;
    bool live_ = false;
    ULONGLONG last_match_ = 0, window_start_ = 0;
    double window_out_ = 0;
    mutable double last_y_ = 0, last_p_ = 0;
    mutable bool have_last_ = false;
    double pushed_pitch_ = 0;  // the gyro's pitch pushes in this window (game sense, radians)
    int blocked_pitch_ = 0;  // +1/-1: the game's pitch limit is that way (game sense)
};

}  // namespace

Engine& engine() {
    static ToylogicEngine e;
    return e;
}

}  // namespace gyro4all::toylogic
