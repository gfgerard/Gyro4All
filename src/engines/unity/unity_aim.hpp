#pragma once
// The parts of the Unity view's checks with no game access (unit-tested).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gyro4all::unity {

// "Does this camera answer the player's look input?", from 2 s windows. A fixed scene camera (top-down, scripted) never
// turns while the player gives look input; an aim camera does. Three consecutive still windows park the gyro; two
// consecutive answering ones bring it back. An orthographic camera needs no verdict: the view just never uses one.
struct AimCheck {
    static constexpr double kStill = 0.5;  // degrees per window: a camera that turns this little isn't answering
    static constexpr int kStillWindows = 3, kResponsiveWindows = 2;
    static constexpr uint64_t kLookMs = 300;  // look input for less than this in a window is no evidence (a bumped mouse)
    static constexpr double kOwnMotion = 2;   // turning this many times faster with look input than without: it answers

    bool unresponsive() const { return unresponsive_; }

    // moved: how far the game turned the camera for the look input this window (degrees, look_motion), not counting the
    // mod's own writes. look: the player gave look input the game can see, for kLookMs or more. Other windows keep the
    // streaks as they are.
    void window(double moved, bool look) {
        if (!look) return;
        if (moved < kStill) {
            responsive_ = 0;
            if (++still_ >= kStillWindows) unresponsive_ = true;
        } else {
            still_ = 0;
            if (unresponsive_ && ++responsive_ >= kResponsiveWindows) unresponsive_ = false;
        }
    }

    void reset() { *this = {}; }

private:
    int still_ = 0, responsive_ = 0;
    bool unresponsive_ = false;
};

// What a window's look input turned the camera: looked_deg over look_ms, unless the camera turns about as fast by itself
// (idle_deg_per_s, measured without look input): a cutscene's pan goes on whatever the player does.
inline double look_motion(double looked_deg, uint64_t look_ms, double idle_deg_per_s) {
    if (look_ms == 0) return 0;
    return looked_deg / (double(look_ms) / 1000) >= AimCheck::kOwnMotion * idle_deg_per_s ? looked_deg : 0;
}

// A camera nobody is turning stays put: a cutscene or a title shot pans. Idle sway stays under kMoved.
struct ViewStillness {
    static constexpr double kMoved = 0.5;  // degrees from where it settled

    void reset() { since_ = 0; }

    // True once the view has stayed within kMoved for hold_ms. now is never 0.
    bool add(uint64_t now, double yaw, double pitch, uint64_t hold_ms) {
        if (!since_ || std::abs(std::remainder(yaw - yaw_, 360.0)) > kMoved || std::abs(pitch - pitch_) > kMoved) {
            yaw_ = yaw;
            pitch_ = pitch;
            since_ = now;
        }
        return now - since_ >= hold_ms;
    }

private:
    uint64_t since_ = 0;
    double yaw_ = 0, pitch_ = 0;
};

// A view found with no player input is only a guess until the player turns the camera: if the camera turns with the
// stick or mouse and the view's state doesn't, that state is something else (an effect on top of the real one).
// Fed with the frames where the player turned and the gyro didn't. Two "ignored" verdicts in a row reject it.
struct LookStateCheck {
    static constexpr double kEnough = 20, kFollows = 0.25, kCut = 20;  // degrees; a jump this big in a frame is a shot cut
    enum class Verdict { None, Follows, Rejected };

    Verdict add(double camera_deg, double state_deg) {
        if (done_ || std::abs(camera_deg) >= kCut) return Verdict::None;
        camera_ += std::abs(camera_deg);
        state_ += std::abs(state_deg);
        if (camera_ < kEnough) return Verdict::None;
        const bool follows = state_ >= kFollows * camera_;
        camera_ = state_ = 0;
        if (follows) {
            done_ = true;
            return Verdict::Follows;
        }
        return ++ignored_ >= 2 ? Verdict::Rejected : Verdict::None;
    }

private:
    double camera_ = 0, state_ = 0;
    int ignored_ = 0;
    bool done_ = false;
};

// The game's pitch limit, learnt from next frame's rotation: writing the Transform bypasses the game's own clamp, and
// a game that clamps pulls the write back in its update. A limit is believed after kFrames frames in a row at the same
// pitch, and given up as soon as the camera is seen past it (the game moved it there itself).
struct PitchClamp {
    static constexpr int kFrames = 3;
    double up = 89, down = -89;

    // wrote: the pitch written last frame; got: the pitch read this frame. True when a limit changed.
    bool observe(double wrote, double got) {
        if (got > up + 0.5) up = std::min(got, 89.0);
        if (got < down - 0.5) down = std::max(got, -89.0);
        const bool pulled = std::abs(wrote) > 45 && std::signbit(got) == std::signbit(wrote) &&
                            std::abs(got) < std::abs(wrote) - 0.5 && std::abs(got) > 20;
        if (!pulled || (run_ > 0 && std::abs(got - last_) > 0.5)) run_ = 0;
        if (!pulled) return false;
        last_ = got;
        if (++run_ < kFrames) return false;
        run_ = 0;
        double& limit = wrote > 0 ? up : down;
        if (std::abs(limit - got) < 0.5) return false;
        limit = got;
        return true;
    }

private:
    int run_ = 0;
    double last_ = 0;
};

// How much of a pitch turn fits under the limits. A camera already past one (the game put it there) stays where it is
// until the turn comes back, rather than jumping to the limit.
inline double limit_pitch_turn(double current, double turn, double down, double up) {
    if (turn > 0) return current >= up ? 0 : std::min(turn, up - current);
    if (turn < 0) return current <= down ? 0 : std::max(turn, down - current);
    return 0;
}

}  // namespace gyro4all::unity
