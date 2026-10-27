#pragma once
// Profile views with Property=RotationInput (the stick's look input of the PlayerController): the gyro is added to
// RotationInput inside the controller's UpdateRotation, the function that turns the camera from it, so the gyro takes the
// stick's own path (the game's limits, the pawn turned with its attached camera and mesh). For games that rebuild
// ControlRotation from elsewhere every frame (Dead Island 2: its yaw from the character's capsule). A write from the
// message pump can't do it: the game zeroes RotationInput at the start of the player tick.
// UpdateRotation is found in the controller's vtable by its shape (ue_update_rotation_match.hpp), not by address.
#include "ue.hpp"

#include <string>

namespace gyro4all::unreal::update_rotation {

// A RotationInput view: the gyro goes through UpdateRotation.
bool is_input_property(const std::string& property);

// Where RotationInput is in the controller. It isn't reflected in UE4 (a plain member), so it is found next to the reflected
// InputYawScale, declared right after it in APlayerController (InputYawScale_DEPRECATED in UE5), with ControlRotation's
// rotator size; a reflected RotationInput wins. Logged once.
bool rotation_input(ue::Obj pc, int32_t& offset, int32_t& size);

// Queues the turn for the next UpdateRotation of `pc` (hooked on first use for its class). False when UpdateRotation
// wasn't found (logged once per controller class).
bool add(ue::Obj pc, int32_t rotation_input_offset, int32_t rotator_size, double pitch, double yaw);

// The turn queued and not yet taken by UpdateRotation (degrees).
void pending(ue::Obj pc, double& pitch, double& yaw);

}  // namespace gyro4all::unreal::update_rotation
