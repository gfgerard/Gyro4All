#pragma once
// Match=Camera views (engines/unrealEngine/ue_profile.hpp): which of several rotations turns the camera of one view
// target right now. Engine-neutral (no game memory access), unit-tested (tests/profile_tests.cpp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gyro4all {

// The members are the base view (the ViewTargetClass view, or the default one) and the Match=Camera views that apply
// to the same view target. The gyro is written into the current member's rotation, and the member is dropped for the
// next one as soon as the game shows it isn't the camera's input:
// - fast: the game puts the rotation back to its old value after each write (it rewrites it from the real input, an
//   output now: PAYDAY 3's ControlRotation during a clamped action), a few frames in a row;
// - its object is missing (the clamp modifier outside its action): at once;
// - slow: the camera ignores the gyro written into it, over a short time window (a rotation the game neither
//   rewrites nor reads). The window tolerates a camera that follows with some lag.
// Both paths judge yaw only: at the pitch limit (looking straight up or down) every camera ignores pitch writes, which
// isn't a camera change.
// The gyro lost on a dropped member is handed to the next one (carry_yaw/carry_pitch), so a switch delays the turn
// instead of eating it. After every member was dropped, the one the camera followed last is used (else the base view),
// and the group rests a moment before trying again (a cutscene ignores them all, and so does a camera held at a yaw
// limit; nothing is carried then).
class CameraMatch {
public:
    enum Event { None, Switched, Follows, AllIgnored };
    enum Reason { Ignored, Rewritten, Missing };      // why the last member was dropped
    static constexpr double kJudgeDegrees = 2;        // slow path: gyro written into the member before it is judged
    static constexpr uint64_t kJudgeMinMs = 200;      // ... over at least this long (camera lag, smoothing)
    static constexpr uint64_t kWindowMaxMs = 1500;    // a window with less gyro than that is dropped
    static constexpr double kIgnoredRatio = 0.2, kFollowsRatio = 0.5;
    static constexpr int kRewrittenFrames = 3;        // fast path: writes put back in a row
    static constexpr uint64_t kRestMs = 2000;
    static constexpr double kMaxCarryYaw = 45;        // degrees handed to the next member at most
    static constexpr double kMaxCarryPitch = 10;      // (pitch: it may have been pushed against the limit)

    struct Frame {
        double yaw = 0, pitch = 0;  // gyro written into the current member this frame (or lost, with write_failed)
        double moved = 0;           // the camera's yaw rotation this frame (degrees, absolute)
        bool look = false;          // right stick or mouse: the camera's motion can't be judged
        bool write_failed = false;  // the member's rotation couldn't be written (its object is missing)
        int rewritten = -1;         // the previous frame's write: 1 = put back by the game, 0 = kept, -1 = unknown
    };

    // The game's answer to a yaw write of `written` degrees into a rotation that held `before`, `after` once written,
    // and `now` a frame later: 1 = put back (rewritten from elsewhere), 0 = kept, -1 = can't tell (a small write, or
    // the value moved for other reasons). Yaw only: at the pitch limit the game clamps pitch writes, which is no
    // rewrite.
    static int rewritten(double before, double after, double now, double written) {
        const double w = std::abs(written);
        if (w < 0.05) return -1;
        const double back = std::abs(wrap180(now - before)), kept = std::abs(wrap180(now - after));
        if (back < 0.25 * w && kept > 0.75 * w) return 1;
        if (kept < 0.25 * w) return 0;
        return -1;
    }

    // The view id to use this frame (members[0] = the base view). Starts over when the members change.
    uint32_t select(const std::vector<uint32_t>& members) {
        if (members != members_) {
            members_ = members;
            index_ = failed_ = followed_ = 0;
            switched_ = false;
            rest_until_ = 0;
            reset_window();
            reset_streak();
            unclear_yaw_ = unclear_pitch_ = 0;
        }
        return members_.empty() ? 0 : members_[index_];
    }

    // More than one member: worth reading the rotation back for the fast path.
    bool active() const { return members_.size() > 1; }

    Event frame(const Frame& f, uint64_t now_ms) {
        const double last_yaw = last_yaw_, last_pitch = last_pitch_;
        last_yaw_ = f.yaw, last_pitch_ = f.pitch;
        carry_yaw_ = carry_pitch_ = 0;
        if (members_.size() < 2 || now_ms < rest_until_) return None;
        if (f.write_failed) {
            reason_ = Missing;
            return advance(now_ms, f.yaw, f.pitch);
        }
        if (f.look) {
            reset_window();
            reset_streak();
            unclear_yaw_ = unclear_pitch_ = 0;  // the player aimed again: an old loss isn't theirs to get back
            return None;
        }
        // Fast path: the previous frame's write was put back.
        if (f.rewritten == 1) {
            ++streak_;
            streak_yaw_ += last_yaw, streak_pitch_ += last_pitch;
            if (streak_ >= kRewrittenFrames) {
                reason_ = Rewritten;
                return advance(now_ms, unclear_yaw_ + streak_yaw_ + f.yaw, unclear_pitch_ + streak_pitch_ + f.pitch);
            }
        } else if (f.rewritten == 0) {
            reset_streak();
        }
        // Slow path: the camera's yaw against the gyro's yaw written, over a window.
        const double applied = std::abs(f.yaw);
        if (!window_start_) {
            if (applied <= 0) return None;
            window_start_ = now_ms;
        }
        applied_ += applied;
        moved_ += f.moved;
        window_yaw_ += f.yaw, window_pitch_ += f.pitch;
        const uint64_t age = now_ms - window_start_;
        if (applied_ < kJudgeDegrees || age < kJudgeMinMs) {
            if (age > kWindowMaxMs) reset_window();
            return None;
        }
        const double ratio = moved_ / applied_;
        // The share of this window's gyro that didn't reach the camera.
        const double lost = std::clamp(1 - ratio, 0.0, 1.0);
        const double lost_yaw = window_yaw_ * lost, lost_pitch = window_pitch_ * lost;
        reset_window();
        if (ratio < kIgnoredRatio) {
            reason_ = Ignored;
            return advance(now_ms, unclear_yaw_ + lost_yaw, unclear_pitch_ + lost_pitch);
        }
        if (ratio <= kFollowsRatio) {
            // Unclear (the switch happened inside this window, or a laggy camera): keep its lost share for a switch.
            unclear_yaw_ += lost_yaw, unclear_pitch_ += lost_pitch;
            return None;
        }
        // Followed: the earlier losses are settled, but this window may already hold the first ignored frames of a
        // camera change (followed for 150 ms, ignored for 50): its lost share waits for a switch.
        unclear_yaw_ = lost_yaw, unclear_pitch_ = lost_pitch;
        failed_ = 0;
        followed_ = index_;
        if (switched_) {
            switched_ = false;
            return Follows;
        }
        return None;
    }

    uint32_t current() const { return members_.empty() ? 0 : members_[index_]; }
    uint32_t previous() const { return previous_; }  // the member before the last switch
    Reason reason() const { return reason_; }
    // After Switched: the gyro the dropped member lost, to write into the new one this frame (0 otherwise).
    double carry_yaw() const { return carry_yaw_; }
    double carry_pitch() const { return carry_pitch_; }

private:
    static double wrap180(double a) {
        a = std::fmod(a, 360.0);
        if (a > 180) a -= 360;
        if (a < -180) a += 360;
        return a;
    }
    Event advance(uint64_t now_ms, double lost_yaw, double lost_pitch) {
        reset_window();
        reset_streak();
        unclear_yaw_ = unclear_pitch_ = 0;
        last_yaw_ = last_pitch_ = 0;  // this frame's write went to the member being dropped
        previous_ = current();
        switched_ = true;
        if (++failed_ >= members_.size()) {
            index_ = followed_;
            failed_ = 0;
            rest_until_ = now_ms + kRestMs;
            return AllIgnored;
        }
        index_ = (index_ + 1) % members_.size();
        carry_yaw_ = std::clamp(lost_yaw, -kMaxCarryYaw, kMaxCarryYaw);
        carry_pitch_ = std::clamp(lost_pitch, -kMaxCarryPitch, kMaxCarryPitch);
        return Switched;
    }
    void reset_window() {
        applied_ = moved_ = window_yaw_ = window_pitch_ = 0;
        window_start_ = 0;
    }
    void reset_streak() {
        streak_ = 0;
        streak_yaw_ = streak_pitch_ = 0;
    }

    std::vector<uint32_t> members_;
    size_t index_ = 0, failed_ = 0;
    size_t followed_ = 0;  // the member the camera followed last
    uint32_t previous_ = 0;
    bool switched_ = false;
    Reason reason_ = Ignored;
    uint64_t rest_until_ = 0, window_start_ = 0;
    double applied_ = 0, moved_ = 0, window_yaw_ = 0, window_pitch_ = 0;
    int streak_ = 0;
    double streak_yaw_ = 0, streak_pitch_ = 0;
    double last_yaw_ = 0, last_pitch_ = 0;
    double carry_yaw_ = 0, carry_pitch_ = 0;
    double unclear_yaw_ = 0, unclear_pitch_ = 0;  // lost share of the last followed window and unclear ones since
};

}  // namespace gyro4all
