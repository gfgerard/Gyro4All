// Unity (Mono scripting backend): MOUSE: P.I. For Hire first. The game is detected by <exe stem>_Data and UnityPlayer.dll
// next to the exe. The exe is a small stub; UnityPlayer.dll imports PeekMessageW (and winmm.dll/version.dll, so the
// proxy loads from the exe's folder), so it is hooked as a game module (prefix "UnityPlayer"). Its main thread pumps the
// window's messages, then runs the frame's scripts: the gyro frame at the end of the pump runs right before the
// game's Update, on the thread Mono runs the scripts on.
//
// No engine-wide camera exists in Unity (each game rotates its own transforms), so the camera comes from the game's
// profile ([Unity], unity_path.hpp): the player object (a static field or property, e.g. a singleton's Instance), the
// transform whose world yaw (eulerAngles.y) turns and the one whose local pitch (localEulerAngles.x; Unity's positive
// pitch looks down) turns, as member paths read through Mono's reflection (unity_mono.hpp). The turn is written through
// Transform's own setters, as the game's look code does, before the game's Update reads them back and adds the stick.
// Pause is Time.timeScale 0; a mouse cursor that isn't locked (and visible, where the build keeps that getter) is a
// menu. Without a profile, or in an IL2CPP build (no Mono), the gyro goes to the game as right-stick input (the core's
// blind stick fallback). MODLOG 2026-11-11 has the findings.
#include "unity.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "unity_mono.hpp"
#include "unity_path.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace gyro4all::unity {

namespace {

constexpr wchar_t kSection[] = L"Unity";
constexpr double kDefaultPitchLimit = 89;

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Degrees to (-180, 180].
double wrap180(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180) a -= 360;
    else if (a <= -180) a += 360;
    return a;
}

// The profile's [Unity] section, parsed.
struct Profile {
    bool camera = false;  // Player= is set: the gyro turns the game's transforms
    mono::Path player, yaw, pitch, pitch_min, pitch_max, camera_object, zoom_reference;
    std::vector<mono::Path> look;  // all true: the gameplay view
};

std::optional<mono::Path> profile_path(const wchar_t* key, const wchar_t* fallback, bool& bad) {
    const std::string text = settings::profile_string(kSection, key, fallback);
    std::string error;
    auto p = parse_path(text, error);
    if (!p) {
        log("unity: profile [Unity] %s=%s: %s", narrow(key).c_str(), text.c_str(), error.c_str());
        bad = true;
        return std::nullopt;
    }
    return mono::Path(std::move(*p));
}

class UnityEngine final : public Engine {
public:
    const char* name() const override { return "Unity"; }

    // <exe stem>_Data (the player's data) and UnityPlayer.dll (Unity 2017.2 and later) next to the exe; also Unity's
    // crash handler there (is_launcher), so it stays a plain proxy.
    bool detect(const std::wstring& exe_path) const override {
        const size_t slash = exe_path.find_last_of(L"\\/");
        const std::wstring dir = exe_path.substr(0, slash);
        std::wstring stem = exe_path.substr(slash + 1);
        stem = stem.substr(0, stem.find_last_of(L'.'));
        return exists(dir + L"\\UnityPlayer.dll") && (exists(dir + L"\\" + stem + L"_Data") || is_launcher(exe_path));
    }

    // UnityCrashHandler64.exe: the player starts it from the game's folder at launch (MODLOG 2026-11-11). Without this,
    // the core took it for the real game started by a launcher and handed it the session, leaving the game without the
    // gyro and the panel.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring exe = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        return _wcsicmp(exe.c_str(), L"UnityCrashHandler64.exe") == 0 ||
               _wcsicmp(exe.c_str(), L"UnityCrashHandler32.exe") == 0;
    }

    std::wstring module_prefix(const std::wstring&) const override { return L"UnityPlayer"; }

    // UnityPlayer.dll reads gamepads through Windows.Gaming.Input and only when that fails loads XInput (and looks its
    // functions up with GetProcAddress): XInput=2 refuses the former so the game gets the Xbox controller shown to it.
    bool wgi_falls_back_to_xinput() const override { return true; }

    const wchar_t* window_class() const override { return L"UnityWndClass"; }

    void configure() override { load_profile("start"); }

    void settings_changed() override { load_profile("profile saved"); }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, zoom_available()}}; }

    bool views_changed() override {
        const bool changed = views_changed_;
        views_changed_ = false;
        return changed;
    }

    EngineFrame read_frame() override {
        EngineFrame f;
        frame_ok_ = false;
        const bool ready = bound();
        if (ready) {
            float scale = 1;
            if (mono::time_scale(scale))
                set_flag(paused_, scale < 1e-6f, "time stopped (Time.timeScale 0: paused)", "time running");
            bool free = false;
            if (mono::cursor_free(free)) set_flag(cursor_, free, "mouse cursor free (menu)", "mouse cursor locked");
        }
        f.paused = paused_;
        f.cursor = cursor_;
        // No camera for this game (no profile, IL2CPP): the gyro as right stick, still held by pause and cursor once
        // the runtime is bound. A profiled game waits for the runtime instead.
        if (!profile_.camera || runtime_ == mono::Runtime::Unsupported) return blind(f);
        if (!ready) return f;

        mono::Value player;
        const bool found = profile_.player.resolve(nullptr, player) && player.object;
        if (found != player_found_) {
            log("unity: player %s", found ? ("found: " + mono::class_name(player.object)).c_str()
                                          : ("gone (" + profile_.player.why_not() + ")").c_str());
            player_found_ = found;
        }
        if (!found) return f;
        player_ = player.object;
        f.in_game = true;
        f.target = mono::class_name(player_);

        std::string why;
        const bool look = look_allowed(why);
        mono::Value yaw_obj, pitch_obj;
        yaw_t_ = profile_.yaw.resolve(player_, yaw_obj) ? mono::transform_of(yaw_obj.object) : nullptr;
        pitch_t_ = profile_.pitch.resolve(player_, pitch_obj) ? mono::transform_of(pitch_obj.object) : nullptr;
        if (!yaw_t_ || !pitch_t_) why = !yaw_t_ ? "no yaw transform (" + profile_.yaw.why_not() + ")"
                                                : "no pitch transform (" + profile_.pitch.why_not() + ")";
        const bool view = look && yaw_t_ && pitch_t_ && read_angles();
        if (view != view_) {
            log("unity: gameplay view %s", view ? "on (the gyro turns the player's transforms)" : ("off: " + why).c_str());
            view_ = view;
        }
        frame_ok_ = view;
        f.view_id = view ? 1 : 0;
        if (view) read_limits();
        if (view && zoom_available()) f.zoom_fov = zoom_fov();
        return f;
    }

    void limit_turn(double& pitch, double&) override {
        if (!frame_ok_) return;
        // Unity pitch = current - gyro pitch, kept within [min, max].
        pitch = std::clamp(pitch, pitch_ - max_pitch_, pitch_ - min_pitch_);
    }

    bool apply(double pitch, double yaw) override {
        if (!frame_ok_) return false;
        bool wrote = false;
        if (yaw != 0) {
            float e[3];
            if (!mono::euler_angles(yaw_t_, false, e)) return false;
            e[1] = float(wrap180(e[1] + yaw));
            if (!mono::set_euler_angles(yaw_t_, false, e)) return false;
            yaw_ = e[1];
            wrote = true;
        }
        if (pitch != 0) {
            float e[3];
            if (!mono::euler_angles(pitch_t_, true, e)) return wrote;
            const double x = std::clamp(wrap180(e[0]) - pitch, min_pitch_, max_pitch_);
            e[0] = float(x);
            if (!mono::set_euler_angles(pitch_t_, true, e)) return wrote;
            pitch_ = x;
            wrote = true;
        }
        return wrote || (pitch == 0 && yaw == 0);
    }

    Recenter recenter_support(std::string& detail) override {
        if (!frame_ok_) {
            detail = "no gameplay view";
            return Recenter::NotYet;
        }
        detail = std::format("pitch {:.1f}, level 0", -pitch_);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        if (!frame_ok_) return false;
        float e[3];
        if (!mono::euler_angles(pitch_t_, true, e)) return false;
        const double x = wrap180(e[0]);
        e[0] = float(x - x * std::clamp(fraction, 0.0, 1.0));
        if (!mono::set_euler_angles(pitch_t_, true, e)) return false;
        pitch_ = e[0];
        return true;
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (!frame_ok_ || !read_angles()) return false;
        pitch = -pitch_;
        yaw = yaw_;
        camera = reinterpret_cast<uintptr_t>(player_);
        return true;
    }

    std::string status_scene() const override {
        if (runtime_ != mono::Runtime::Ready) return "unity: " + runtime_detail_;
        if (!profile_.camera) return "unity: no [Unity] Player= in the game's profile (gyro as right stick)";
        if (!player_found_) return "unity: no player (" + profile_.player.why_not() + ")";
        return std::format("unity: view {} pitch {:.1f} yaw {:.1f} (limits {:.0f}..{:.0f}){}{}", view_ ? "on" : "off",
                           -pitch_, yaw_, -max_pitch_, -min_pitch_, paused_ ? " paused" : "", cursor_ ? " cursor" : "");
    }

private:
    bool zoom_available() const { return profile_.camera && !profile_.camera_object.empty(); }

    // The runtime bound (asked again every 500 ms until it is, logged when that changes).
    bool bound() {
        if (runtime_ == mono::Runtime::Ready) return true;
        if (runtime_ == mono::Runtime::Unsupported) return false;
        const ULONGLONG now = GetTickCount64();
        if (now - last_bind_ < 500) return false;
        last_bind_ = now;
        std::string detail;
        runtime_ = mono::bind(detail);
        if (detail != runtime_detail_ || runtime_ != mono::Runtime::NotLoaded) {
            if (runtime_ == mono::Runtime::Ready) log("unity: Mono runtime and UnityEngine bound");
            else log("unity: %s%s", detail.c_str(),
                     runtime_ == mono::Runtime::Unsupported ? "; the gyro goes to the game as right-stick input" : "");
        }
        runtime_detail_ = runtime_ == mono::Runtime::Ready ? "ready" : detail;
        return runtime_ == mono::Runtime::Ready;
    }

    // No camera for this game: the default view with the core's blind stick fallback.
    EngineFrame& blind(EngineFrame& f) {
        f.no_camera = true;
        f.in_game = true;
        f.view_id = 1;
        f.target = "no camera hook (gyro as right stick)";
        return f;
    }

    void set_flag(bool& flag, bool value, const char* on, const char* off) {
        if (value != flag) log("unity: %s", value ? on : off);
        flag = value;
    }

    // Every Look= condition holds.
    bool look_allowed(std::string& why) {
        for (mono::Path& c : profile_.look) {
            mono::Value v;
            if (!c.resolve(player_, v) || !v.is_number) {
                why = describe(c.spec()) + " can't be read" + (c.why_not().empty() ? "" : " (" + c.why_not() + ")");
                return false;
            }
            if ((v.number != 0) == c.spec().negate) {
                why = describe(c.spec()) + " is false";
                return false;
            }
        }
        return true;
    }

    bool read_angles() {
        float y[3], p[3];
        if (!mono::euler_angles(yaw_t_, false, y) || !mono::euler_angles(pitch_t_, true, p)) return false;
        yaw_ = wrap180(y[1]);
        pitch_ = wrap180(p[0]);
        return std::isfinite(yaw_) && std::isfinite(pitch_);
    }

    // PitchMin=/PitchMax= (Unity's degrees, positive looks down); kept apart by at least a degree.
    void read_limits() {
        mono::Value lo, hi;
        const bool ok = profile_.pitch_min.resolve(player_, lo) && lo.is_number && profile_.pitch_max.resolve(player_, hi) &&
                        hi.is_number && hi.number - lo.number >= 1 && lo.number >= -90 && hi.number <= 90;
        const double min = ok ? lo.number : -kDefaultPitchLimit, max = ok ? hi.number : kDefaultPitchLimit;
        if (min != min_pitch_ || max != max_pitch_)
            log("unity: pitch limits %.1f..%.1f (Unity degrees, positive down)%s", min, max, ok ? "" : ", the default");
        min_pitch_ = min;
        max_pitch_ = max;
    }

    // The camera's vertical FOV (Camera.fieldOfView) and the unzoomed one: ZoomReference= (vertical degrees), else
    // [Gyro4All] ZoomReferenceFOV= (horizontal, converted with the window's aspect). Changes of the current one logged.
    std::optional<ZoomFov> zoom_fov() {
        mono::Value cam;
        float current = 0;
        if (!profile_.camera_object.resolve(player_, cam) || !mono::camera_fov(cam.object, current)) return std::nullopt;
        double reference = 0;
        mono::Value ref;
        if (!profile_.zoom_reference.empty() && profile_.zoom_reference.resolve(player_, ref) && ref.is_number)
            reference = ref.number;
        else if (const double h = fov::reference_setting(); h > 0)
            reference = fov::vertical_from_horizontal(h, fov::game_window_aspect());
        if (!fov::is_valid(current) || !fov::is_valid(reference)) return std::nullopt;
        if (std::abs(current - logged_fov_) > 0.5 || std::abs(reference - logged_reference_) > 0.5) {
            if (fov_logs_ < 40) log("unity: camera FOV %.1f degrees vertical (unzoomed %.1f)", current, reference);
            ++fov_logs_;
            logged_fov_ = current;
            logged_reference_ = reference;
        }
        return ZoomFov{current, reference};
    }

    void load_profile(const char* why) {
        const bool zoom_was = zoom_available();
        Profile p;
        bool bad = false;
        const std::string player = settings::profile_string(kSection, L"Player");
        p.camera = !player.empty();
        if (p.camera) {
            auto root = profile_path(L"Player", L"", bad);
            if (root && root->spec().from_player()) {
                log("unity: profile [Unity] Player=%s: needs a static member (Assembly:Class::Member)", player.c_str());
                bad = true;
            }
            auto yaw = profile_path(L"Yaw", L"", bad);
            auto pitch = profile_path(L"Pitch", L"", bad);
            auto lo = profile_path(L"PitchMin", L"-89", bad);
            auto hi = profile_path(L"PitchMax", L"89", bad);
            auto cam = profile_path(L"Camera", L"", bad);
            auto zoom = profile_path(L"ZoomReference", L"", bad);
            std::vector<MemberPath> look;
            std::string error;
            const std::string look_text = settings::profile_string(kSection, L"Look");
            if (!parse_path_list(look_text, look, error)) {
                log("unity: profile [Unity] Look=%s: %s", look_text.c_str(), error.c_str());
                bad = true;
            }
            if (!bad) {
                p.player = std::move(*root);
                p.yaw = std::move(*yaw);
                p.pitch = std::move(*pitch);
                p.pitch_min = std::move(*lo);
                p.pitch_max = std::move(*hi);
                // An empty Camera=/ZoomReference= parses as "the player": none.
                if (!settings::profile_string(kSection, L"Camera").empty()) p.camera_object = std::move(*cam);
                if (!settings::profile_string(kSection, L"ZoomReference").empty()) p.zoom_reference = std::move(*zoom);
                for (MemberPath& m : look) p.look.emplace_back(std::move(m));
            }
        }
        if (bad) {
            log("unity: the profile's [Unity] section has errors (%s): no camera, the gyro goes to the game as "
                "right-stick input", why);
            p = Profile{};
        } else if (p.camera) {
            std::string conditions;
            for (const mono::Path& c : p.look) conditions += (conditions.empty() ? "" : ", ") + describe(c.spec());
            log("unity: profile (%s): player %s, yaw %s, pitch %s, pitch limits %s..%s, view while %s, camera %s, "
                "unzoomed FOV %s",
                why, describe(p.player.spec()).c_str(), describe(p.yaw.spec()).c_str(), describe(p.pitch.spec()).c_str(),
                describe(p.pitch_min.spec()).c_str(), describe(p.pitch_max.spec()).c_str(),
                conditions.empty() ? "the player exists" : conditions.c_str(),
                p.camera_object.empty() ? "none" : describe(p.camera_object.spec()).c_str(),
                p.zoom_reference.empty() ? "ZoomReferenceFOV=" : describe(p.zoom_reference.spec()).c_str());
        } else {
            log("unity: no [Unity] Player= in the game's profile (%s): no camera, the gyro goes to the game as "
                "right-stick input", why);
        }
        profile_ = std::move(p);
        frame_ok_ = false;
        view_ = false;
        player_found_ = false;
        if (zoom_available() != zoom_was) views_changed_ = true;
    }

    Profile profile_;
    mono::Runtime runtime_ = mono::Runtime::NotLoaded;
    std::string runtime_detail_ = "Mono runtime not bound yet";
    ULONGLONG last_bind_ = 0;
    bool views_changed_ = false;

    // This frame's scene (valid from read_frame to apply: no script runs between them).
    mono::Object* player_ = nullptr;
    mono::Object* yaw_t_ = nullptr;
    mono::Object* pitch_t_ = nullptr;
    bool frame_ok_ = false;
    double pitch_ = 0, yaw_ = 0;  // Unity's: pitch positive down, yaw positive right
    double min_pitch_ = -kDefaultPitchLimit, max_pitch_ = kDefaultPitchLimit;

    bool player_found_ = false, view_ = false, paused_ = false, cursor_ = false;
    double logged_fov_ = 0, logged_reference_ = 0;
    int fov_logs_ = 0;
};

}  // namespace

Engine& engine() {
    static UnityEngine instance;
    return instance;
}

}  // namespace gyro4all::unity
