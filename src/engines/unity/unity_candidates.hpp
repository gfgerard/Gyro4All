#pragma once
// The camera probe's arithmetic, with no game access (unit-tested): which float in the game's scripts moves with the
// camera, how many of its units make a degree, and what the verified result writes into the profile.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "unity_fov.hpp"

namespace gyro4all::unity {

// How a value moved against the camera over the samples taken while the player turned it.
struct SlotStats {
    double value2 = 0, value_yaw = 0, value_pitch = 0;
    int moved = 0;  // samples where the value changed

    void add(double value, double yaw, double pitch) {
        if (value != 0) ++moved;
        value2 += value * value;
        value_yaw += value * yaw;
        value_pitch += value * pitch;
    }
};

struct AxisFit {
    double correlation = 0;  // -1..1
    double per_degree = 0;   // the value's units for one degree of the camera (negative: it runs the other way)
};

// sum: the value's product with this axis (SlotStats::value_yaw or value_pitch); axis2: the axis' own sum of squares.
inline AxisFit fit_axis(double sum, double value2, double axis2) {
    if (value2 <= 0 || axis2 <= 0) return {};
    return {sum / std::sqrt(value2 * axis2), sum / axis2};
}

// A measured scale, snapped to degrees or radians when it is one of them.
inline double snap_per_degree(double per_degree) {
    constexpr double kRadian = 3.14159265358979323846 / 180;
    const double size = std::abs(per_degree), sign = per_degree < 0 ? -1 : 1;
    if (size > 0.95 && size < 1.05) return sign;
    if (size > kRadian * 0.95 && size < kRadian * 1.05) return sign * kRadian;
    return per_degree;
}

// How good a candidate looks: 0 = no candidate. Tracks the axis closely, not the other one, and moved in enough samples;
// real angles in degrees or radians first (a 0..1 axis like Cinemachine's FreeLook still qualifies, behind them).
inline double candidate_score(const AxisFit& axis, const AxisFit& other, int moved) {
    constexpr int kMinMoved = 5;
    constexpr double kMinCorrelation = 0.9, kMaxOther = 0.5;
    const double size = std::abs(axis.per_degree);
    if (moved < kMinMoved || std::abs(axis.correlation) < kMinCorrelation || std::abs(other.correlation) > kMaxOther ||
        size < 1e-4 || size > 100)
        return 0;
    const double snapped = std::abs(snap_per_degree(axis.per_degree));
    const double unit_bonus = snapped == 1 ? 0.2 : snapped != size ? 0.1 : 0;
    return std::abs(axis.correlation) + unit_bonus;
}

// "m_HorizontalAxis.Value" -> m, horizontal, axis, value. Names are matched by word, or "min" would hit "m_Input".
inline std::vector<std::string> name_words(const std::string& name) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) words.push_back(word);
        word.clear();
    };
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) {
            flush();
            continue;
        }
        const bool next_lower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1]));
        if (std::isupper(c) && !word.empty() && (std::islower(static_cast<unsigned char>(name[i - 1])) || next_lower)) flush();
        word += char(std::tolower(c));
    }
    flush();
    return words;
}

// A script worth searching further: a camera, look or player script holds its state deeper than the rest do.
inline bool camera_class_name(const std::string& class_name) {
    constexpr const char* kClass[] = {"camera", "cam", "look", "head", "aim", "view", "player", "mouse", "fps", "orbit", "cinemachine"};
    for (const std::string& w : name_words(class_name))
        for (const char* item : kClass)
            if (w == item) return true;
    return false;
}

// How much a script's float looks like the player's look state, before anything is measured: the order the search with
// no player input tries them in. 0 = not worth touching (a setting, a limit, a copy of an old value, an effect on top).
inline int discovery_score(const std::string& class_name, const std::string& path, bool on_camera_rig) {
    constexpr const char* kNot[] = {"speed",    "sensitivity", "sens",     "min",       "max",      "minimum", "maximum", "limit",
                                    "limits",   "clamp",       "range",    "damp",      "damping",  "smooth",  "smoothing",
                                    "velocity", "accel",       "acceleration", "time",  "timer",    "duration", "delay",  "distance",
                                    "dist",     "fov",         "radius",   "offset",    "shake",    "recoil",  "bob",     "sway",
                                    "noise",    "lean",        "kick",     "weight",    "blend",    "threshold", "scale", "deadzone",
                                    "height",   "volume",      "alpha",    "gain",      "multiplier", "factor", "lerp",   "rate",
                                    "curve",    "strength",    "amount",   "cooldown",  "length",   "width",   "size",    "snap",
                                    "last",     "prev",        "previous", "old",       "initial",  "default", "original", "start",
                                    "ratio",    "semitone",    "intensity"};
    constexpr const char* kStrong[] = {"pitch", "yaw"};
    constexpr const char* kLook[] = {"rot",  "rotation", "angle", "angles", "heading", "elevation", "azimuth", "tilt",
                                     "pan",  "axis",     "horizontal", "vertical", "look", "aim",   "euler",   "orbit",
                                     "degrees", "degree"};  // Outer Wilds' PlayerCameraController._degreesY
    constexpr const char* kWeak[] = {"x", "y", "view", "mouse"};
    const auto has = [](const std::vector<std::string>& words, const auto& list) {
        for (const std::string& w : words)
            for (const char* item : list)
                if (w == item) return true;
        return false;
    };
    // "modes[SpeedData].pitch": what is in the brackets is a class name, not a word about this value.
    std::string name_only;
    bool inside = false;
    for (const char c : path) {
        if (c == '[') inside = true;
        if (!inside) name_only += c;
        if (c == ']') inside = false, name_only += '.';
    }
    const std::vector<std::string> words = name_words(name_only);
    if (has(words, kNot)) return 0;
    const bool klass = camera_class_name(class_name);
    int name = has(words, kStrong) ? 8 : has(words, kLook) ? 4 : has(words, kWeak) ? 1 : 0;
    if (!on_camera_rig && (name == 0 || (name == 1 && !klass))) return 0;
    return name + (klass ? 4 : 0) + (on_camera_rig ? 4 : 0);
}

// A nudge of 0.25 on an angle in radians is 14.3 degrees. More than that is no angle: an input or a speed, which the game goes
// on adding every frame (Viewfinder's PlayerMovement._lookInputVector spun the player by 49 degrees).
inline constexpr double kNudgeMaxTurn = 16;

// The float's units for a degree, from a small nudge: the camera's turn must be about one axis. 0 = it isn't.
inline double nudge_per_degree(double step, double moved_axis, double moved_other) {
    constexpr double kMinTurn = 0.05, kMaxOther = 0.25;
    if (std::abs(moved_axis) < kMinTurn || std::abs(moved_axis) > kNudgeMaxTurn ||
        std::abs(moved_other) > kMaxOther * std::abs(moved_axis))
        return 0;
    return step / moved_axis;
}

// The camera's turn answered a value written live: within the tolerance on its axis, and the other axis stayed put.
inline bool turn_followed(double asked, double moved, double moved_other) {
    constexpr double kTolerance = 1.0;
    return std::abs(moved - asked) <= kTolerance && std::abs(moved_other) <= kTolerance;
}

// A look state on a Transform a script holds (PitchObject=/YawObject=): how its own turn compares with the camera's over
// the player's turns. It is in degrees by nature, so only about one degree per degree, the same way, qualifies; anything the
// camera hangs from or follows does, which the live test sorts out (the game must read it back and keep a turn).
inline double transform_score(const AxisFit& axis, const AxisFit& other, int moved) {
    if (axis.per_degree < 0.8 || axis.per_degree > 1.25) return 0;
    return candidate_score(axis, other, moved);
}

// Which of the Transforms the scene's scripts hold are worth a live test with no player input, by the name of the field
// that holds them and its script's class: likeliest first, 0 = not tried. Pitch lives on a head or camera rig, yaw on the
// body (MOUSE: P.I. For Hire's Player.cameraRigController and Player.characterController).
inline int transform_discovery_score(const std::string& class_name, const std::string& field, bool pitch_axis) {
    constexpr const char* kNot[] = {"weapon", "gun", "hand", "hands", "arm", "arms", "target", "enemy", "spawn", "muzzle", "ui",
                                    "canvas", "hud", "icon", "door", "item", "pickup", "light", "effect", "fx", "audio", "sound",
                                    "foot", "feet", "ground", "last", "prev", "previous", "old", "start", "default", "original"};
    constexpr const char* kPitch[] = {"head", "rig", "pivot", "neck", "pitch", "camera", "cam", "eye", "eyes", "look", "view", "aim"};
    constexpr const char* kYaw[] = {"body", "character", "controller", "player", "root", "yaw", "pawn", "avatar"};
    const auto has = [](const std::vector<std::string>& words, const auto& list) {
        for (const std::string& w : words)
            for (const char* item : list)
                if (w == item) return true;
        return false;
    };
    const std::vector<std::string> words = name_words(field);
    if (words.empty() || has(words, kNot)) return 0;
    // A head turns with the body, and a game that writes only its pitch back would keep a yaw put on it: a body comes first.
    const int name = pitch_axis ? (has(words, kPitch) ? 8 : 0) : (has(words, kYaw) ? (has(words, kPitch) ? 2 : 8) : 0);
    if (name == 0) return 0;
    return name + (camera_class_name(class_name) ? 4 : 0);
}

// The Rejected= entry of an object path: "<class> @<path>".
inline std::string object_rejection(const std::string& class_name, const std::string& path) { return class_name + " @" + path; }

struct ProbeResult {
    std::string pitch_class, pitch_path, yaw_class, yaw_path, camera_field;  // pitch_class "": pitch on the camera's Transform
    double pitch_per_degree = 0, yaw_per_degree = 0;
    int yaw_parent = 0;      // > 0: yaw on that ancestor's Transform (the body), levels above the camera; no yaw float
    bool automatic = false;  // found with no player input: to be confirmed by the first real stick or mouse turn
    bool pitch_transform = false;  // the game copies the pitch float to the camera only on look input: turn both
    std::string pitch_object, yaw_object;  // a path to a component or GameObject whose Transform has it, instead of a float
    std::string zoom_reference;            // ZoomReference=: a float path from ObjectType's object to the unzoomed FOV
    FovAxis zoom_axis = FovAxis::Auto;     // the axis it matched the camera's FOV in
};

// The [Unity] keys of a verified result.
inline std::vector<std::pair<std::string, std::string>> profile_keys(const ProbeResult& r) {
    std::vector<std::pair<std::string, std::string>> keys;
    if (!r.pitch_class.empty()) {
        keys.emplace_back("ObjectType", r.pitch_class);
        if (!r.yaw_parent && !r.yaw_class.empty() && r.yaw_class != r.pitch_class) keys.emplace_back("YawObjectType", r.yaw_class);
        if (!r.camera_field.empty()) keys.emplace_back("CameraField", r.camera_field);
        if (!r.pitch_object.empty()) {
            keys.emplace_back("PitchObject", r.pitch_object);
        } else {
            keys.emplace_back("PitchPath", r.pitch_path);
            keys.emplace_back("PitchPerDegree", std::format("{:.7g}", snap_per_degree(r.pitch_per_degree)));
            if (r.pitch_transform) keys.emplace_back("PitchTransform", "1");
        }
    }
    if (r.yaw_parent > 0) {
        keys.emplace_back("YawParent", std::to_string(r.yaw_parent));
    } else if (!r.yaw_object.empty()) {
        keys.emplace_back("YawObject", r.yaw_object);
    } else {
        keys.emplace_back("YawPath", r.yaw_path);
        keys.emplace_back("YawPerDegree", std::format("{:.7g}", snap_per_degree(r.yaw_per_degree)));
    }
    if (!r.pitch_class.empty() && !r.zoom_reference.empty()) {
        keys.emplace_back("ZoomReference", r.zoom_reference);
        if (r.zoom_axis != FovAxis::Auto) keys.emplace_back("ZoomReferenceAxis", fov_axis_name(r.zoom_axis));
    }
    if (r.automatic) keys.emplace_back("Source", "auto");
    return keys;
}

// A float the game keeps equal to the camera's pitch, for a view whose pitch is on the camera's Transform: the game's own
// look state, which it may copy to the camera only on look input (The Planet Crafter's PlayerLookable._pitch). Fed after
// the player's own turns; each float is tried in degrees and radians, counting up or down, and as an angle kept in 0..360.
class PitchStateMatch {
public:
    static constexpr double kRadian = 3.14159265358979323846 / 180;
    static constexpr double kScales[4] = {-1, 1, -kRadian, kRadian};  // units per degree up; Unity's Euler x counts down
    static constexpr int kMinMatches = 5;
    static constexpr double kMinRange = 10, kTolerance = 0.05;  // degrees

    struct Found {
        size_t index = 0;
        double per_degree = 0;
    };

    void reset(size_t count) { cells_.assign(count * 4, {}); }

    // values: every float now (NaN: unreadable); pitch_up: the camera's pitch, degrees.
    void add(const std::vector<float>& values, double pitch_up) {
        for (size_t i = 0; i < values.size() && i * 4 < cells_.size(); ++i)
            for (size_t s = 0; s < 4; ++s) {
                Cell& c = cells_[i * 4 + s];
                if (c.out) continue;
                if (std::isfinite(values[i]) && std::abs(wrap(values[i] / kScales[s] - pitch_up)) <= kTolerance) {
                    ++c.matches;
                    c.low = std::min(c.low, pitch_up), c.high = std::max(c.high, pitch_up);
                } else {
                    ++c.misses;
                }
                // A copy that lags a smoothed camera may miss a few; anything else misses most.
                c.out = c.misses > 3 && c.misses * 4 > c.matches;
            }
    }

    // Matched often enough, over enough of the pitch range, and missed little.
    std::vector<Found> found() const {
        std::vector<Found> out;
        for (size_t k = 0; k < cells_.size(); ++k) {
            const Cell& c = cells_[k];
            if (!c.out && c.matches >= kMinMatches && c.high - c.low >= kMinRange && c.misses * 4 <= c.matches)
                out.push_back({k / 4, kScales[k % 4]});
        }
        return out;
    }

    // Nothing left that could still match.
    bool hopeless() const {
        return std::all_of(cells_.begin(), cells_.end(), [](const Cell& c) { return c.out; });
    }

private:
    struct Cell {
        int matches = 0, misses = 0;
        double low = 1e9, high = -1e9;
        bool out = false;
    };
    static double wrap(double degrees) { return std::remainder(degrees, 360.0); }
    std::vector<Cell> cells_;
};

}  // namespace gyro4all::unity
