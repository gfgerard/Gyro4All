#pragma once
// Control's gameplay camera (control_camera.cpp): the look direction of GamePlayerCameraMovementComponentState, found by
// its RTTI vtable and a heap scan on a thread of its own, read as pitch/heading and turned in place.
#include <cstdint>

namespace gyro4all::northlight::control {

enum class Search { NotStarted, Searching, NoVtable, Done };

// Game thread, every frame: starts the search (vtable once, then the heap scan) unless one runs, at most once a
// second. Returns where it stands.
Search poll();
// The live component found by the last scan, 0 if none (still valid when returned).
uintptr_t component();
// Forgets the component (it went away: loading, a new level), so poll() scans again.
void lost();
// The component is still there: its vtable and a unit look direction at +0xB8 (the scan also checks its copies).
bool valid(uintptr_t c);
// The look direction's pitch (degrees, up positive) and heading (degrees, grows turning right).
bool look(uintptr_t c, double& pitch, double& heading);
// Turns the look direction by pitch (up) and yaw (right) degrees, the pitch kept within [-max_down, max_up].
bool turn(uintptr_t c, double pitch, double yaw, double max_up, double max_down);
// The component's per-frame update is hooked (found by the scan thread): turns go through add() and are applied there.
bool hooked();
// Game thread: adds a turn (degrees) for the update hook to apply to the live component on its next run, within the pitch
// limits. A turn left untaken for 100 ms (pause, loading) is dropped first.
void add(double pitch, double yaw, double max_up, double max_down);
// The pitch (degrees) added and not applied yet.
double pending_pitch();
}  // namespace gyro4all::northlight::control
