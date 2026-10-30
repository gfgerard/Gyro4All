#pragma once
// What Northlight's camera hooks share (control_camera.cpp, aw2_camera.cpp): finding code in the exe, turning a look
// direction in place, and the queue that hands the gyro's turns from Gyro4All's frame to the game's camera update.
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gyro4all::northlight {

struct Section {
    uintptr_t begin = 0, end = 0;  // the part initialized from the file
};

// The exe's section of this name, {0, 0} if none.
Section exe_section(const char* name);
// A copy of the section (empty when unreadable).
std::vector<uint8_t> copy(const Section& s);
// The start of the function holding code at pc, through the unwind table (chained entries followed to the primary).
uintptr_t function_start(uintptr_t pc);
// Where code matching pattern (any_at: one byte that matches anything, or SIZE_MAX) is in the exe's .text; 0 unless it
// is there exactly once. hits: how many times it was.
uintptr_t find_unique_code(const uint8_t* pattern, size_t size, size_t any_at, int& hits);

// A unit float 3-vector at a, finite.
bool read3(uintptr_t a, double v[3]);
double length(const double v[3]);
// A look direction (float x, y, z; world +Y up, heading atan2(x, z)) at a: its pitch (degrees, up positive) and heading
// (degrees, grows turning right).
bool look_angles(uintptr_t a, double& pitch, double& heading);
// Turns the look direction at a by pitch (up) and yaw (right) degrees, the pitch kept within [-max_down, max_up].
bool turn_look(uintptr_t a, double pitch, double yaw, double max_up, double max_down);

// The gyro's turns from Gyro4All's frame (add) to the game's camera update (take, on the game's job thread), as running
// totals at the times of Gyro4All's frames. The two run out of step (MODLOG 2026-10-30, Control), so take() reads the
// totals at its own frame clock: the game's frame time when the hook has it, else the average update interval, pulled
// gently toward real time minus a delay (6 ms, or 1.5 times Gyro4All's frame spacing when more), interpolated between
// Gyro4All's frames, so each update gets an even share. A turn left untaken for 100 ms (pause, loading) is dropped.
class LookQueue {
public:
    // Gyro4All's frame: adds a turn (degrees) and the pitch limits to apply it within.
    void add(double pitch, double yaw, double max_up, double max_down);
    // The camera update: what to turn by now (degrees) and the limits. False when there is nothing to turn. dt: the game's
    // frame time for this update (seconds), 0 when unknown.
    bool take(double& pitch, double& yaw, double& max_up, double& max_down, double dt = 0);
    // The pitch (degrees) added and not taken yet.
    double pending_pitch();
    // GetTickCount64 of the last take(), 0 if none.
    ULONGLONG last_take() const { return last_take_.load(); }

private:
    struct Total {
        int64_t t = 0;  // QueryPerformanceCounter
        double pitch = 0, yaw = 0;
    };
    Total totals_at(int64_t t) const;
    int64_t frame_clock(double dt);

    std::mutex mutex_;  // guards everything below but last_take_
    Total totals_[16];  // the last frames' totals, totals_[last_] the newest
    int last_ = 0;
    double applied_pitch_ = 0, applied_yaw_ = 0, max_up_ = 80, max_down_ = 80;
    int64_t freq_ = 0;
    int64_t clock_ = 0, last_call_ = 0;  // frame_clock
    double interval_ = 0;
    double add_spacing_ = 0;  // average QueryPerformanceCounter units between add() calls
    std::atomic<ULONGLONG> last_take_{0};
};

}  // namespace gyro4all::northlight
