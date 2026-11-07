#pragma once
// The camera probe (see unity_probe.cpp): finds the float the player's look input turns, tests it live and writes the
// game's profile. Game thread, except probe_configure. Results in Gyro4All\Gyro4All-probe.log.
namespace gyro4all::unity {

class Runtime;

const char* probe_default_settings();  // its commented Probe=/ProbeKey=/AutoProbe= block for a new Gyro4All.ini
// At start and when the settings change. wanted_by_profile: Probe=1 in the profile's [Unity]. Going from off to on starts
// the probe.
void probe_configure(bool wanted_by_profile);
// Once per frame. gameplay: focus, no pause, no cursor, no panel.
void probe_frame(Runtime& runtime, bool gameplay);
bool probe_active();
bool probe_holds_gyro();  // the gyro must not turn the camera while it measures
// Finds the camera with no player input (the file's comment, "With no input"). Every frame on the generic view. allowed: a
// perspective camera in gameplay with the mouse captured. Goes on watching quietly when nothing is found.
void probe_discover(Runtime& runtime, void* camera, bool allowed);
bool probe_discovery_pending();  // no search has run yet: the gyro waits a moment rather than bend the camera first
// A UI element has the pad's or keyboard's focus (for up to a minute per camera): a menu, whatever the cursor says.
// Nothing searches it, and the generic view keeps off its camera.
bool probe_menu_on_screen();
// The verdict windows keep running: the probe only watches, in the background.
bool probe_watching_quietly();
// The first real turn showed that a view found by itself is wrong: it may search again, without it.
void probe_discovery_again();
// The camera stopped answering the gyro: starts the probe unless AutoProbe=0, a probe runs, 3 automatic starts were used
// or the last one ended less than 30 s ago. True if started (its own notice says what to do).
bool probe_request_auto(Runtime& runtime, const char* why);
// A view with its pitch on the camera's Transform (YawParent=, no pitch float): every frame. sample: the player's own look
// input moved the view last frame, so the game's state and the camera agree now. A float of the scripts on the camera's rig
// that always matches its pitch goes into the profile (PitchPath=, PitchTransform=1). Once per camera; nothing while the
// probe runs.
void probe_pitch_state(Runtime& runtime, void* camera, int yaw_parent, bool sample, double pitch_up);

}  // namespace gyro4all::unity
