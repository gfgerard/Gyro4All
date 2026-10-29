#pragma once
// Sensor-time pacing (SensorPacing=1, MODLOG 2026-10-29): GyroLib's turn for a frame covers the sensor time of the
// reports read in that frame, and SDL reads them at gl_update, so a frame gets whatever arrived (8-12 ms of motion in a
// 10 ms frame at 100 FPS with a DualSense over USB): the turn per frame jitters even at a steady hand. The pacer plays
// the turn back in sensor time instead: each frame gets the motion up to (frame time - delay), the same span of motion
// as the frame's own length, as the game's stick does with its frame time.
//
// - Sensor time to host time: sensor + the lowest arrival-minus-sensor offset of the last kOffsetWindowNs (the
//   transport's least latency; follows the two clocks' drift).
// - Delay: by default (delay_ns < 0) the median of how far the last report lags each frame, so the average latency
//   stays where it was without pacing; past the last report the last rate is extrapolated (at most kMaxExtrapolateNs:
//   reports sometimes stop for over 10 ms in the middle of a fast turn, MODLOG 2026-10-29).
// - Never against the motion: a frame's turn on an axis is zero or has the sign of the sensor motion at the target
//   time; an extrapolation the reports then undercut is owed back during the next motion, at most half of each frame's
//   turn, never as a step back.
// - While GyroLib spreads bursts itself (a turn in an update without new reports: its burst delivery, Bluetooth pads
//   that send reports in groups) its turn goes through as it is. After a stall (a frame or a gap in the reports over
//   kStallNs) what is owed and the update's turn go out at once. reset() (gate closed, the sensor's clock restarted)
//   drops what hasn't been played back yet, as GyroLib does with its pending motion.
// Engine-neutral, no GyroLib or game access: host.cpp feeds it GyroLib's samples and output; unit-tested.
#include <cstdint>
#include <deque>

namespace gyro4all {

class SensorPacer {
public:
    struct Settings {
        bool on = true;
        int64_t delay_ns = -1;  // < 0: automatic (median lag of the last report)
    };

    void configure(const Settings& s);
    const Settings& settings() const { return s_; }
    // A report GyroLib accepted (its sample observer), before the frame() of the update that read it.
    void sample(uint64_t sensor_ns, uint64_t arrival_ns);
    // One update: GyroLib's turn in, the turn to apply this frame out (degrees).
    void frame(uint64_t now_ns, double& pitch, double& yaw);
    void reset();

    double delay_ms() const { return double(delay_ns_) * 1e-6; }
    // Since the last call: frames paced, those of them that extrapolated past the last report, frames left to
    // GyroLib's burst delivery, and stalls (turned at once).
    void take_counts(uint32_t& paced, uint32_t& extrapolated, uint32_t& bursts, uint32_t& stalls) {
        paced = paced_, extrapolated = extrapolated_, bursts = bursts_, stalls = stalls_;
        paced_ = extrapolated_ = bursts_ = stalls_ = 0;
    }

    static constexpr int64_t kOffsetWindowNs = 2'000'000'000;
    static constexpr int64_t kMaxDelayNs = 8'000'000, kStartDelayNs = 2'000'000, kDelayStepNs = 50'000;
    static constexpr int64_t kMaxExtrapolateNs = 20'000'000;
    static constexpr int64_t kStallNs = 100'000'000;  // a gap in frames or reports this long starts over
    static constexpr int64_t kKeepNs = 200'000'000;    // timeline kept behind the target time
    static constexpr uint64_t kBurstHoldNs = 1'000'000'000;

private:
    struct Point {
        int64_t host_ns;
        double pitch, yaw;  // cumulative turn up to here
    };
    struct Offset {
        int64_t arrival_ns, offset_ns;
    };

    // The cumulative turn at host time x (extrapolated past the last point; extrapolated set then) and the sensor
    // motion's rate there (degrees per ns), per axis.
    void position(int64_t x, double& pitch, double& yaw, double& slope_pitch, double& slope_yaw, bool& extrapolated) const;
    static double pace_axis(double target, double& emitted, int sign, int& last_sign, double motion);

    Settings s_;
    std::deque<Offset> offsets_;  // increasing offsets: a monotonic queue for the window's minimum
    std::deque<Point> points_;
    // This update's reports.
    uint32_t new_samples_ = 0;
    uint64_t first_sensor_ = 0, last_sensor_ = 0;
    uint64_t prev_sensor_ = 0;  // the last report of the previous updates
    uint64_t last_frame_ns_ = 0;
    int64_t delay_ns_ = kStartDelayNs;
    double cum_pitch_ = 0, cum_yaw_ = 0;          // GyroLib's turn so far (paced part)
    double emitted_pitch_ = 0, emitted_yaw_ = 0;  // turn played back so far
    int last_sign_pitch_ = 0, last_sign_yaw_ = 0;
    bool timeline_ = false;
    uint64_t burst_until_ = 0;  // GyroLib's burst delivery seen: no pacing until then
    uint32_t paced_ = 0, extrapolated_ = 0, bursts_ = 0, stalls_ = 0;
};

}  // namespace gyro4all
