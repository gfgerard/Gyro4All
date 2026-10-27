#pragma once
// Self-timing of Gyro4All's own work, for benchmarking the mod's cost:
//   idle    = Gyro4All's game-thread work in frames where the gyro did not move the camera;
//   active  = the same, in frames where it applied a rotation (gyro in use);
//   present = what the Present hook adds on the render thread (panel; ~0 while closed).
// Each window is summarised (count, avg, p99, max in microseconds) by perf_report().
#include <cstdint>

namespace gyro4all::perf {

enum Counter { Idle = 0, Active = 1, Present = 2, Count };

int64_t now();                         // QPC ticks
void record(Counter c, int64_t start);  // thread-safe; adds now() - start
void report();                          // logs and resets the window (call from the game thread)

}  // namespace gyro4all::perf
