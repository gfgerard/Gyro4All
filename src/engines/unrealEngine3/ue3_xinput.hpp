#pragma once
// UE3's list of connected controllers (ue3_xinput.cpp).
namespace gyro4all::unreal3 {

// Game thread: the game builds its controller list again (XInput slots that answer now), as when its viewport was
// created. Found on the first call; does nothing if it wasn't found.
void recheck_controllers();

}  // namespace gyro4all::unreal3
