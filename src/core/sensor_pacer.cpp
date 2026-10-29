#include "sensor_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace gyro4all {

namespace {

constexpr double kStillDegPerNs = 0.01 * 1e-9;  // slower than 0.01 deg/s: no motion on that axis
constexpr int64_t kDefaultFrameNs = 10'000'000, kMaxFrameNs = 50'000'000;

int sign_of(double slope) { return slope > kStillDegPerNs ? 1 : slope < -kStillDegPerNs ? -1 : 0; }

}  // namespace

void SensorPacer::configure(const Settings& s) {
    s_ = s;
    if (s_.delay_ns > kMaxDelayNs) s_.delay_ns = kMaxDelayNs;
    offsets_.clear();
    burst_until_ = 0;
    reset();
}

void SensorPacer::reset() {
    points_.clear();
    timeline_ = false;
    new_samples_ = 0;
    first_sensor_ = last_sensor_ = prev_sensor_ = 0;
    last_frame_ns_ = 0;
    cum_pitch_ = cum_yaw_ = emitted_pitch_ = emitted_yaw_ = 0;
    last_sign_pitch_ = last_sign_yaw_ = 0;
}

void SensorPacer::sample(uint64_t sensor_ns, uint64_t arrival_ns) {
    if (!s_.on) return;
    const uint64_t latest = std::max(last_sensor_, prev_sensor_);
    if (latest && sensor_ns <= latest) {
        // The sensor's clock restarted (GyroLib rejects duplicates and reports out of order): a new mapping.
        offsets_.clear();
        reset();
    }
    const int64_t offset = int64_t(arrival_ns) - int64_t(sensor_ns);
    while (!offsets_.empty() && offsets_.back().offset_ns >= offset) offsets_.pop_back();
    offsets_.push_back({int64_t(arrival_ns), offset});
    while (offsets_.front().arrival_ns < int64_t(arrival_ns) - kOffsetWindowNs) offsets_.pop_front();
    if (!new_samples_++) first_sensor_ = sensor_ns;
    last_sensor_ = sensor_ns;
}

void SensorPacer::position(int64_t x, double& pitch, double& yaw, double& slope_pitch, double& slope_yaw,
                           bool& extrapolated) const {
    extrapolated = false;
    slope_pitch = slope_yaw = 0;
    if (points_.empty()) {
        pitch = yaw = 0;
        return;
    }
    if (points_.size() == 1 || x <= points_.front().host_ns) {
        pitch = points_.front().pitch, yaw = points_.front().yaw;
        if (points_.size() == 1 || x < points_.front().host_ns) return;
    }
    // The segment that holds x (the last one past the end).
    size_t k = points_.size() - 1;
    while (k > 1 && points_[k - 1].host_ns > x) --k;
    const Point& a = points_[k - 1];
    const Point& b = points_[k];
    const int64_t span = b.host_ns - a.host_ns;
    // A report span of zero length (a single report) is a step: its motion over a millisecond for the slope.
    const double over = double(span > 0 ? span : 1'000'000);
    slope_pitch = (b.pitch - a.pitch) / over;
    slope_yaw = (b.yaw - a.yaw) / over;
    if (x >= b.host_ns) {
        const int64_t ahead = std::min(x - b.host_ns, kMaxExtrapolateNs);
        pitch = b.pitch + slope_pitch * double(ahead);
        yaw = b.yaw + slope_yaw * double(ahead);
        extrapolated = ahead > 0;
        return;
    }
    const double f = span > 0 ? double(x - a.host_ns) / double(span) : 1;
    pitch = a.pitch + (b.pitch - a.pitch) * f;
    yaw = a.yaw + (b.yaw - a.yaw) * f;
}

// One axis: the turn this frame toward target (cumulative), given the sensor motion's sign at the target time and the
// turn a frame of this length makes at that rate (motion, >= 0). With the motion: what is owed, within half and one
// and a half of motion (an extrapolation overshot is paid back at half speed, a lag caught up at most half again as
// fast). Without motion: what is still owed in the direction the motion had, else nothing.
double SensorPacer::pace_axis(double target, double& emitted, int sign, int& last_sign, double motion) {
    const double owed = target - emitted;
    double turn = 0;
    if (sign != 0) {
        turn = sign * std::clamp(owed * sign, 0.5 * motion, 1.5 * motion);
        last_sign = sign;
    } else if (last_sign != 0 && owed * last_sign > 0) {
        turn = owed;
    }
    emitted += turn;
    return turn;
}

void SensorPacer::frame(uint64_t now_ns, double& pitch, double& yaw) {
    if (!s_.on) return;
    const int64_t now = int64_t(now_ns);
    const int64_t frame_ns = last_frame_ns_ && now_ns > last_frame_ns_ ? int64_t(now_ns - last_frame_ns_) : 0;
    last_frame_ns_ = now_ns;
    // A turn without new reports: GyroLib spreads this controller's bursts over the updates itself (its burst delivery,
    // Bluetooth pads that send reports in groups). Not paced a second time: GyroLib's turn as it is, until kBurstHoldNs
    // without such updates.
    if (!new_samples_ && (pitch != 0 || yaw != 0)) burst_until_ = now_ns + kBurstHoldNs;
    const bool burst = now_ns < burst_until_;
    // A stall (a long frame, or a long gap in the reports): already a jump on screen, so it is not spread over the
    // next frames as well (a 140 ms stall's turn took that long again).
    const bool stall = frame_ns > kStallNs || (new_samples_ && prev_sensor_ && first_sensor_ - prev_sensor_ > uint64_t(kStallNs));
    if (burst || stall) {
        // What is owed goes out with this update's turn; the timeline starts over from its last report.
        if (timeline_) {
            pitch += cum_pitch_ - emitted_pitch_;
            yaw += cum_yaw_ - emitted_yaw_;
            emitted_pitch_ = cum_pitch_, emitted_yaw_ = cum_yaw_;
            points_.clear();
            timeline_ = false;
        }
        if (new_samples_) prev_sensor_ = last_sensor_;
        new_samples_ = 0;
        ++(burst ? bursts_ : stalls_);
        return;
    }
    double through_pitch = pitch, through_yaw = yaw;
    if (new_samples_ && !offsets_.empty()) {
        const int64_t offset = offsets_.front().offset_ns;
        int64_t end = std::min(int64_t(last_sensor_) + offset, now);
        if (!timeline_) {
            // From the last report already turned (after a burst or a stall), else this update's first.
            const int64_t start = std::min(int64_t(prev_sensor_ ? prev_sensor_ : first_sensor_) + offset, end);
            points_.clear();
            points_.push_back({start, cum_pitch_, cum_yaw_});
            emitted_pitch_ = cum_pitch_;
            emitted_yaw_ = cum_yaw_;
            last_sign_pitch_ = last_sign_yaw_ = 0;
            timeline_ = true;
        }
        end = std::max(end, points_.back().host_ns);
        cum_pitch_ += pitch;
        cum_yaw_ += yaw;
        points_.push_back({end, cum_pitch_, cum_yaw_});
        through_pitch = through_yaw = 0;
        if (s_.delay_ns < 0) {
            // Median tracking of the last report's lag behind the frame.
            delay_ns_ += now - end > delay_ns_ ? kDelayStepNs : -kDelayStepNs;
            delay_ns_ = std::clamp<int64_t>(delay_ns_, 0, kMaxDelayNs);
        }
        prev_sensor_ = last_sensor_;
    }
    new_samples_ = 0;
    if (s_.delay_ns >= 0) delay_ns_ = s_.delay_ns;
    pitch = through_pitch, yaw = through_yaw;
    if (!timeline_) return;
    const int64_t x = now - delay_ns_;
    double tp = 0, ty = 0, slope_p = 0, slope_y = 0;
    bool extrapolated = false;
    position(x, tp, ty, slope_p, slope_y, extrapolated);
    const double dt = double(frame_ns > 0 ? std::min(frame_ns, kMaxFrameNs) : kDefaultFrameNs);
    pitch += pace_axis(tp, emitted_pitch_, sign_of(slope_p), last_sign_pitch_, std::abs(slope_p) * dt);
    yaw += pace_axis(ty, emitted_yaw_, sign_of(slope_y), last_sign_yaw_, std::abs(slope_y) * dt);
    while (points_.size() > 2 && points_[1].host_ns < x - kKeepNs) points_.pop_front();
    ++paced_;
    extrapolated_ += extrapolated;
}

}  // namespace gyro4all
