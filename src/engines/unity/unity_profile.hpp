#pragma once
#include <string>
#include <vector>

namespace gyro4all::unity {

// A profile's [Unity] section: the managed state the player's look input turns, for games whose view isn't Camera.main's
// Transform (the game rebuilds it from that state, or Camera.main is another camera). The camera probe writes it.
struct UnityProfile {
    std::string object_type;      // ObjectType: the class of the object the paths start from
    std::string yaw_object_type;  // YawObjectType: YawPath's class when it differs ("" = ObjectType)
    std::string camera_field;     // CameraField: path to the Camera (FOV, follow check); "" = Camera.main
    std::vector<std::string> path;        // PitchYawPath: a value type with float pitch and yaw fields
    std::vector<std::string> pitch_path;  // PitchPath: a float
    std::vector<std::string> yaw_path;    // YawPath: a float
    // The value's units for one degree of view, up and right (negative: it runs the other way; degrees are +-1, radians
    // +-0.0174533). By default pitch counts down and yaw right, in degrees (Unity's own Euler convention).
    double pitch_per_degree = -1, yaw_per_degree = 1;
    double pitch_limit = 0;  // PitchLimit: degrees up/down; 0 = learn the game's clamp from readbacks, or +-89 on a float
    int yaw_parent = 0;      // YawParent: yaw on the Transform this many levels above the camera (the body), not a float
    // PitchTransform=1: the game copies PitchPath to the camera's Transform only when it turns the view itself (The Planet
    // Crafter's PlayerLookable, on look input only): the gyro turns both, or the next stick turn would undo its pitch.
    bool pitch_transform = false;
    bool auto_found = false;  // Source=auto: found with no player input, confirmed or rejected by the first real turn
    bool probe = false;      // Probe=1 in [Unity]: run the camera probe (like Probe=1 in [Gyro4All])
    // Turning managed floats. YawParent alone is the generic view with its yaw on the body.
    bool enabled() const {
        return !object_type.empty() && (!path.empty() || (!pitch_path.empty() && (!yaw_path.empty() || yaw_parent > 0)));
    }
    bool has_view() const { return enabled() || yaw_parent > 0; }
    bool operator==(const UnityProfile&) const = default;
};

UnityProfile load_unity_profile(const std::wstring& path);

}  // namespace gyro4all::unity
