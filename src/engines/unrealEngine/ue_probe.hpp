#pragma once
// The camera probe (Probe=1, the probe key or AutoProbe), see ue_probe.cpp.
namespace gyro4all::unreal {

struct Scene;

const char* probe_default_settings();         // its commented Probe=/ProbeKey=/AutoProbe= block for a new Gyro4All.ini
void probe_configure(bool profile_existed);   // DllMain: reads Probe=, ProbeKey=, AutoProbe=; opens Gyro4All-probe.log
bool probe_on();                              // the probe runs (Probe=1, or started during play)
bool probe_capturing();                       // the automatic probe runs: the gyro must not turn the camera
void probe_stick_seen();                      // GyroLib reports the right stick (else captures don't filter on it)
void probe_keys(const Scene& s);              // F7 snapshot / F8 diff
void auto_probe(const Scene& s);              // automatic captures and the live test, once per frame
void probe_hotkey(const Scene& s);            // ProbeKey: starts the probe during play, or cancels any probe
void auto_probe_watch();                      // an automatic probe gives up after 45 s without a capture
// AutoProbe: the camera stopped following the gyro (or ignores it from the start). Starts the probe during play unless
// AutoProbe=0, a probe runs, 3 automatic starts were used or the last one ended less than 30 s ago. True if started.
bool request_auto_probe();
void auto_probe_camera_follows();             // the camera follows the gyro again: an automatic probe stops

}  // namespace gyro4all::unreal
