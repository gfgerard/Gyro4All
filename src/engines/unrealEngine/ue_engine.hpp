#pragma once
// Internal to the Unreal module: the scene, the profile's views and how they are turned (ue_engine.cpp), shared
// with the camera probe (ue_probe.cpp).
#include "ue.hpp"
#include "ue_profile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::unreal {

// Everything the frame needs from the engine; any missing link leaves the rest at 0.
struct Scene {
    ue::Obj pc = 0, pawn = 0, view_target = 0, camera = 0;
    bool paused = false, cursor = false;
    bool pov_ok = false;  // the camera's final rotation (PlayerCameraManager POV) was read
    double pov_pitch = 0, pov_yaw = 0;
    bool loc_ok = false;  // ... and its location (probe only)
    double pov_loc[3] = {};
};

// A profile object path: Root (PC, Pawn, Camera, ViewTarget) then steps (object properties or subobjects).
ue::Obj resolve_path(const std::vector<std::string>& path, const Scene& s);
// The object a view turns (its Object= path; default: the PlayerController for the default view, else the view target).
ue::Obj view_object(const View& view, const Scene& s);
// Adds the turn to the view's rotation, as a profile writes it (plus its OnGyro writes). False if nothing was written.
bool apply(View& view, const Scene& s, double pitch, double yaw);
// The view's current rotation (its first rotator for raw offsets).
bool read_view_rotation(const View& v, const Scene& s, double& pitch, double& yaw);
// The pawn's own rotation (root component), NaN if unknown.
double pawn_yaw(const Scene& s);
// The right stick is deflected (and tells the probe that GyroLib reports the stick at all).
bool right_stick_moving();
// The player turns the camera themselves: right stick or mouse.
bool look_input();
// The profile file was rewritten by Gyro4All (a view added by the probe): read it again on the next frame.
void profile_changed();
// A view for the probe's live test of one candidate rotation.
View test_view_for(const std::string& object, const std::string& property, const std::vector<int32_t>& offsets, int size,
                   bool world);

}  // namespace gyro4all::unreal
