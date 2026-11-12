#pragma once
// The game's own unzoomed FOV for zoom compensation (ZoomReference=), with no game access (unit-tested): which axis it is in,
// told by comparing it with the camera's (Camera.fieldOfView, always vertical), and which of the scripts' floats the camera
// probe takes for it. A game may keep it horizontal whatever its field's name: MOUSE: P.I. For Hire's
// CameraFovController._baseVerticalFOV holds 105, the horizontal of the camera's 72.5 vertical at 16:9.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "fov.hpp"

namespace gyro4all::unity {

enum class FovAxis { Auto, Vertical, Horizontal };

// ZoomReferenceAxis=: Vertical, Horizontal, anything else (or empty) Auto.
inline FovAxis parse_fov_axis(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower == "vertical" || lower == "v") return FovAxis::Vertical;
    if (lower == "horizontal" || lower == "h") return FovAxis::Horizontal;
    return FovAxis::Auto;
}

inline const char* fov_axis_name(FovAxis axis) {
    return axis == FovAxis::Vertical ? "Vertical" : axis == FovAxis::Horizontal ? "Horizontal" : "Auto";
}

constexpr double kFovMatch = 0.25;  // degrees: a reference equal to the camera's FOV, in one axis or the other

// Which axis a reference is in when the camera shows it (unzoomed): Auto when it matches neither, or both (a square window).
inline FovAxis fov_axis_seen(double reference, double camera_vertical, double aspect) {
    if (!fov::is_valid(reference) || !fov::is_valid(camera_vertical)) return FovAxis::Auto;
    const double horizontal = fov::vertical_from_horizontal(reference, aspect);
    const bool as_vertical = std::abs(camera_vertical - reference) <= kFovMatch;
    const bool as_horizontal = horizontal > 0 && std::abs(camera_vertical - horizontal) <= kFovMatch;
    if (as_vertical == as_horizontal) return FovAxis::Auto;
    return as_vertical ? FovAxis::Vertical : FovAxis::Horizontal;
}

// The axis told over the frames: the first to kVotes frames, with at least four times the other's (a zoom that happens to
// equal the other axis for a moment doesn't decide it). Auto until then: no zoom compensation from the reference meanwhile.
class FovAxisVote {
public:
    static constexpr int kVotes = 30;

    void reset() { *this = {}; }
    FovAxis add(double reference, double camera_vertical, double aspect) {
        if (decided_ != FovAxis::Auto) return decided_;
        const FovAxis seen = fov_axis_seen(reference, camera_vertical, aspect);
        if (seen == FovAxis::Vertical) ++vertical_;
        if (seen == FovAxis::Horizontal) ++horizontal_;
        if (vertical_ >= kVotes && vertical_ >= 4 * horizontal_) decided_ = FovAxis::Vertical;
        if (horizontal_ >= kVotes && horizontal_ >= 4 * vertical_) decided_ = FovAxis::Horizontal;
        return decided_;
    }
    FovAxis decided() const { return decided_; }

private:
    int vertical_ = 0, horizontal_ = 0;
    FovAxis decided_ = FovAxis::Auto;
};

// A script float the probe may take for the unzoomed FOV: its field (the last part of its path) named for a field of view
// and not for a zoom, a limit or a speed, and equal to the camera's FOV now in one axis. Higher scores are likelier; base,
// default and the like name the unzoomed one.
struct FovReferenceMatch {
    int score = 0;
    FovAxis axis = FovAxis::Auto;
};

inline std::optional<FovReferenceMatch> fov_reference_match(std::string_view path, double value, double camera_vertical,
                                                            double aspect) {
    const size_t dot = path.find_last_of('.');
    std::string field(dot == std::string_view::npos ? path : path.substr(dot + 1));
    std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (field.find("fov") == std::string::npos && field.find("fieldofview") == std::string::npos) return std::nullopt;
    for (const char* other : {"zoom", "aim", "ads", "scope", "sight", "current", "target", "min", "max", "sprint", "run", "dash",
                              "slide", "speed", "rate", "time", "duration", "lerp", "smooth", "damp", "mult", "scale", "offset",
                              "delta", "add", "bonus", "kick", "punch", "shake", "weapon", "gun", "vehicle", "car"})
        if (field.find(other) != std::string::npos) return std::nullopt;
    const FovAxis axis = fov_axis_seen(value, camera_vertical, aspect);
    if (axis == FovAxis::Auto) return std::nullopt;
    FovReferenceMatch match{1, axis};
    for (const char* unzoomed : {"base", "default", "normal", "original", "initial", "start", "setting", "option", "player",
                                 "unzoom", "hip", "world"})
        if (field.find(unzoomed) != std::string::npos) {
            match.score += 2;
            break;
        }
    return match;
}

}  // namespace gyro4all::unity
