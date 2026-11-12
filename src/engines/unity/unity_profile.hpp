#pragma once
#include <string>
#include <vector>

#include "unity_fov.hpp"

namespace gyro4all::unity {

// Look=: one condition, a path to a bool (a field, or a property read through its getter), negated with a leading '!'.
struct LookCondition {
    std::vector<std::string> path;
    bool negate = false;
    bool operator==(const LookCondition&) const = default;
};

// A profile's [Unity] section: the managed state the player's look input turns, for games whose view isn't Camera.main's
// Transform (the game rebuilds it from that state, or Camera.main is another camera). The camera probe writes it.
struct UnityProfile {
    std::string object_type;      // ObjectType: the class of the object the paths start from
    std::string yaw_object_type;  // YawObjectType: YawPath's (or YawObject's) class when it differs ("" = ObjectType)
    std::string camera_field;     // CameraField: path to the Camera (FOV, follow check); "" = Camera.main
    std::vector<std::string> path;        // PitchYawPath: a value type with float pitch and yaw fields
    std::vector<std::string> pitch_path;  // PitchPath: a float
    std::vector<std::string> yaw_path;    // YawPath: a float
    // PitchObject=/YawObject=: a path to a Unity object (component or GameObject) whose Transform holds the angle, for
    // games whose look state is a Transform the game reads back every frame and isn't the camera or one of its parents
    // (MOUSE: P.I. For Hire: the camera rig's pitch, the character controller's yaw; the camera follows through
    // Cinemachine). Used when PitchPath= (YawPath=, YawParent=) is empty.
    std::vector<std::string> pitch_object;
    std::vector<std::string> yaw_object;
    // Look=: comma-separated conditions on ObjectType's object; the gyro turns the view only while all of them hold (the
    // game's own "can look" state, e.g. its Look input action's enabled, off in menus, dialogue and cutscenes).
    std::vector<LookCondition> look;
    // ZoomReference=: the game's unzoomed FOV in degrees, a float path from ObjectType's object or a number; "" = the
    // [Gyro4All] ZoomReferenceFOV= (horizontal). ZoomReferenceAxis=: Vertical or Horizontal; empty: told from the camera's
    // own FOV while unzoomed (unity_fov.hpp).
    std::vector<std::string> zoom_reference;
    double zoom_reference_value = 0;  // ZoomReference= as a number
    FovAxis zoom_axis = FovAxis::Auto;
    // The value's units for one degree of view, up and right (negative: it runs the other way; degrees are +-1, radians
    // +-0.0174533). By default pitch counts down and yaw right, in degrees (Unity's own Euler convention).
    double pitch_per_degree = -1, yaw_per_degree = 1;
    double pitch_limit = 0;  // PitchLimit: degrees up/down; 0 = learn the game's clamp from readbacks, or +-89 on a float
    int yaw_parent = 0;      // YawParent: yaw on the Transform this many levels above the camera (the body), not a float
    // PitchTransform=1: the game copies PitchPath to the camera's Transform only when it turns the view itself (The Planet
    // Crafter's PlayerLookable, on look input only): the gyro turns both, or the next stick turn would undo its pitch.
    bool pitch_transform = false;
    // YawFromCamera=1: YawPath is the view's yaw relative to a body the game turns toward it by itself (Firewatch's camera
    // mode offset, which shrinks as the player model catches up): the view's yaw, for judging whether the game keeps the
    // gyro's writes, is the camera's world yaw, not the value.
    bool yaw_from_camera = false;
    bool auto_found = false;  // Source=auto: found with no player input, confirmed or rejected by the first real turn
    bool probe = false;      // Probe=1 in [Unity]: run the camera probe (like Probe=1 in [Gyro4All])

    bool pitch_on_object() const { return path.empty() && pitch_path.empty() && !pitch_object.empty(); }
    bool yaw_on_object() const { return path.empty() && yaw_path.empty() && yaw_parent == 0 && !yaw_object.empty(); }
    // Turning managed state. YawParent alone is the generic view with its yaw on the body.
    bool enabled() const {
        if (object_type.empty()) return false;
        if (!path.empty()) return true;
        const bool pitch = !pitch_path.empty() || !pitch_object.empty();
        const bool yaw = !yaw_path.empty() || yaw_parent > 0 || !yaw_object.empty();
        return pitch && yaw;
    }
    bool has_view() const { return enabled() || yaw_parent > 0; }
    bool operator==(const UnityProfile&) const = default;
};

UnityProfile load_unity_profile(const std::wstring& path);

// Look='s text: "a.b, !c.d" -> conditions. Empty parts are skipped; false (with nothing kept) on a malformed one.
bool parse_look(const std::string& text, std::vector<LookCondition>& out);

}  // namespace gyro4all::unity
