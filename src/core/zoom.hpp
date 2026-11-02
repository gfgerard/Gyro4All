#pragma once
// Zoom compensation on top of GyroLib's gyro.zoom_compensation (a per-view switch in the panel, "Adapt to Zoom"). GyroLib
// scales the gyro by tan(current / 2) / tan(reference / 2) from the two vertical FOVs the core reports before every
// gl_update. The core adds:
// - ZoomStrength= (percent, default 100): that scale raised to strength / 100. GyroLib's formula is fixed, so the core
//   reports the current FOV that makes GyroLib give the scaled value (fov_for_scale). At 100 a zoom moves the image on
//   screen as fast as the unzoomed view; at 75 each zoom feels faster than that by magnification^(1 - 0.75) (a 13.6x
//   scope 1.9x); 0 turns the FOV's effect off. Idea and numbers from PR #13 (MERLev, Mass Effect 3 LE).
// - Manual zoom, for games whose FOV the engine can't report: an aim button (Hold or Toggle) and an optional cycle button
//   step through zoom levels, each an aiming FOV the player sets (ZoomLevels), ramped over ZoomRampMs (ZoomRamp) so the
//   gyro follows the game's zoom animation instead of jumping.
// The tan ratio is the same for horizontal and vertical FOVs of one screen (tan(v / 2) = tan(h / 2) / aspect), so manual
// zoom needs no window aspect. Engine-neutral, no game memory access, unit-tested (tests/profile_tests.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace gyro4all {

inline constexpr double kHalfDegree = std::numbers::pi / 360;  // half an angle in degrees, as radians

// The gyro's scale for a FOV against the reference (degrees, measured the same way), raised to the strength
// (ZoomStrength / 100). 1 for invalid input.
inline double zoom_scale(double fov, double reference, double strength = 1) {
    if (!(fov > 0 && fov < 179) || !(reference > 0 && reference < 179)) return 1;
    return std::pow(std::tan(fov * kHalfDegree) / std::tan(reference * kHalfDegree), std::max(strength, 0.0));
}

// The FOV to report with the reference so that GyroLib's tan ratio is `scale` (inside the 0..179 degrees GyroLib takes).
inline double fov_for_scale(double reference, double scale) {
    const double fov = std::atan(std::tan(reference * kHalfDegree) * std::max(scale, 0.0)) / kHalfDegree;
    return std::clamp(fov, 0.1, 178.9);
}

// The FOV to report for `fov` so that GyroLib applies zoom_scale(fov, reference, strength).
inline double strengthened_fov(double fov, double reference, double strength) {
    return fov_for_scale(reference, zoom_scale(fov, reference, strength));
}

// Manual zoom's level from the controller: 0 = not aiming, 1..count = the aim and each further zoom. The aim button turns
// level 1 on (held, or pressed once with toggle); each cycle press while aiming goes to the next level, back to 1 after
// the last. Aiming ends when the aim button is released (or pressed again with toggle) or gameplay is interrupted (pause,
// cursor, panel, focus): Gyro4All can't read the game's own aim state, so a toggle is dropped there like the sensitivity
// profiles. remember: the next aim starts at the last level used instead of 1 (games that keep each weapon's zoom); a
// reset press (usually the weapon switch) brings that back to 1, and the current level too while aiming.
class ZoomLevels {
public:
    struct Settings {
        int count = 0;  // zoom levels (0 = manual zoom off)
        bool toggle = false;
        bool remember = false;
    };

    void configure(const Settings& s) {
        settings_ = s;
        on_ = false;
        level_ = 0;
        last_ = 1;
    }
    const Settings& settings() const { return settings_; }

    // One frame: gameplay = aiming may be on now; aim/cycle/reset = those buttons are held now.
    int update(bool gameplay, bool aim, bool cycle, bool reset) {
        aim = aim && gameplay;
        cycle = cycle && gameplay;
        reset = reset && gameplay;
        const bool aim_pressed = aim && !aim_down_, cycle_pressed = cycle && !cycle_down_,
                   reset_pressed = reset && !reset_down_;
        aim_down_ = aim, cycle_down_ = cycle, reset_down_ = reset;
        if (settings_.count <= 0) return level_ = 0;
        if (reset_pressed) {
            last_ = 1;
            if (on_) level_ = 1;
        }
        const bool was_on = on_;
        if (!gameplay) on_ = false;
        else if (!settings_.toggle) on_ = aim;
        else if (aim_pressed) on_ = !on_;
        if (!on_) return level_ = 0;
        if (!was_on) level_ = settings_.remember ? std::clamp(last_, 1, settings_.count) : 1;
        else if (cycle_pressed && settings_.count > 1) level_ = level_ % settings_.count + 1;
        last_ = level_;
        return level_;
    }
    int level() const { return level_; }

private:
    Settings settings_;
    bool on_ = false;
    int level_ = 0, last_ = 1;
    bool aim_down_ = false, cycle_down_ = false, reset_down_ = false;
};

// Moves a scale toward its target over a fixed time, linear in log-scale, so a zoom in and the zoom out back take the
// same time and feel symmetric. Every change of target restarts the ramp from the current value.
class ZoomRamp {
public:
    double step(double target, uint64_t now_ns, uint64_t duration_ns) {
        const double to = std::log(std::max(target, 1e-6));
        if (!started_) {
            started_ = true;
            from_ = to_ = current_ = to;
            start_ns_ = now_ns;
        } else if (std::abs(to - to_) > 1e-12) {
            from_ = current_;
            to_ = to;
            start_ns_ = now_ns;
        }
        const double progress =
            duration_ns == 0 || now_ns >= start_ns_ + duration_ns ? 1 : double(now_ns - start_ns_) / double(duration_ns);
        current_ = from_ + (to_ - from_) * progress;
        return std::exp(current_);
    }
    double value() const { return started_ ? std::exp(current_) : 1; }

private:
    bool started_ = false;
    double from_ = 0, to_ = 0, current_ = 0;
    uint64_t start_ns_ = 0;
};

}  // namespace gyro4all
