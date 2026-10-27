#include "stick_fallback.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace gyro4all {

namespace {

constexpr double kMinStep = 0.11;  // degrees: smaller writes don't count either way
constexpr int kLogFrames = 60;     // a changed model is logged at most this often
constexpr int kSaveFrames = 600;   // and saved at most this often while the fallback runs (also when it stops)
constexpr double kSaveFit = 0.7;     // saved only when it explains this much of the camera's motion
constexpr double kSaveWindows = 300;  // from this many windows of clear deflection

double wrap180(double degrees) { return std::remainder(degrees, 360.0); }

// Whether two models differ enough to log or save again (speeds by more than `share`).
bool differs(const StickModel& a, const StickModel& b, double share) {
    const auto speed = [&](double x, double y) {
        return (x < 0) != (y < 0) || std::abs(x - y) > share * std::max(std::abs(x), std::abs(y));
    };
    return a.radial != b.radial || std::abs(a.deadzone - b.deadzone) > 1e-3 || std::abs(a.curve - b.curve) > 1e-3 ||
           std::abs(a.lag - b.lag) > 1 || speed(a.yaw_speed, b.yaw_speed) || speed(a.pitch_speed, b.pitch_speed);
}

}  // namespace

bool StickFallback::restore(const std::string& text) {
    StickModel m;
    if (!m.parse(text)) return false;
    prior_ = model_ = saved_model_ = m;
    has_saved_ = true;
    return true;
}

void StickFallback::frame_blind(bool on, uint64_t now_ns) {
    write_.valid = false;
    undone_run_ = 0;
    if (!on) {
        if (on_) stop("no gameplay (no camera hook: the stick fallback runs blind)");
        blind_ = true;
        return;
    }
    if (!on_ || !blind_) {
        if (on_) stop("the engine lost its camera hook");
        blind_ = true;
        start("no camera hook for this game, so blind (no angles to learn from)");
    }
    dt_ = last_ns_ && now_ns > last_ns_ ? std::clamp(double(now_ns - last_ns_) * 1e-9, 0.002, 0.1) : 1.0 / 60;
    last_ns_ = now_ns;
    probe_ = false;
    ++frames_;
}

void StickFallback::frame(bool known, double pitch, double yaw, uintptr_t camera, bool player_stick, uint64_t now_ns) {
    if (blind_) {
        blind_ = false;
        if (on_) stop("the engine has a camera hook now");
    }
    const bool wrote = write_.valid;  // last frame's turn went to the camera: this frame's angles include it
    known_ = known;
    pitch_ = pitch;
    yaw_ = yaw;
    camera_ = camera;
    if (!known) {
        write_.valid = false;
        undone_run_ = 0;
        if (on_) stop("no gameplay view whose angles can be read");
        return;
    }
    if (gave_up_camera_ && camera != gave_up_camera_) gave_up_camera_ = 0;
    if (forced_ && !on_ && camera != gave_up_camera_) start("forced on (StickFallbackTest)");
    if (on_) {
        dt_ = last_ns_ && now_ns > last_ns_ ? std::clamp(double(now_ns - last_ns_) * 1e-9, 0.002, 0.1) : 1.0 / 60;
        last_ns_ = now_ns;
    }
    // Last frame's direct write: undone if the angles are where the camera would be without it, kept if where it would
    // be with it (both after the turn our own stick should have made); neither, or the player's stick moving: no verdict.
    // While the fallback runs, the stick's own turn in that frame is taken as last frame's (the stick still carries
    // the gyro's turn on a probe frame, so it stays steady; no model or lag needed).
    if (write_.valid && write_.camera == camera && !player_stick) {
        const double stick_yaw = on_ ? last_turn_yaw_ : 0, stick_pitch = on_ ? last_turn_pitch_ : 0;
        const double step = std::abs(write_.pitch) + std::abs(write_.yaw);
        const double base_pitch = write_.pre_pitch + stick_pitch, base_yaw = write_.pre_yaw + stick_yaw;
        const double undone = std::abs(wrap180(pitch - base_pitch)) + std::abs(wrap180(yaw - base_yaw));
        const double kept =
            std::abs(wrap180(pitch - base_pitch - write_.pitch)) + std::abs(wrap180(yaw - base_yaw - write_.yaw));
        if (step > kMinStep && (undone < 0.5 * step || kept < 0.5 * step)) note_write(undone < kept);
    }
    write_.valid = false;
    if (!on_) return;
    if (camera != fb_camera_) return stop("another camera");
    const double turned_pitch = wrap180(pitch - last_pitch_), turned_yaw = wrap180(yaw - last_yaw_);
    last_pitch_ = pitch;
    last_yaw_ = yaw;
    last_turn_pitch_ = turned_pitch;
    last_turn_yaw_ = turned_yaw;
    // The camera's turn is the stick's alone unless the player's stick moved it (also for the game's lag after), a
    // direct write is in it, or it jumped (a cut, a respawn).
    bool usable = !wrote && frames_ > 0 && dt_ < 0.1 && std::abs(turned_pitch) + std::abs(turned_yaw) < kJump;
    if (player_stick) {
        quiet_ = StickLearner::kMaxLag + 2;
        usable = false;
    } else if (quiet_ > 0) {
        --quiet_;
        usable = false;
    }
    learn(turned_pitch, turned_yaw, usable);
    // A stick that turns nothing, judged on clear horizontal deflection only (looking up against the pitch limit turns
    // nothing either); never while forced (a test run keeps going).
    if (!player_stick && !effect_seen_ && !forced_ && std::abs(stick_x_) > 0.5f) {
        effect_turned_ += std::abs(turned_pitch) + std::abs(turned_yaw);
        if (effect_turned_ >= 1) {
            effect_seen_ = true;
        } else if (++effect_frames_ >= kEffectFrames) {
            gave_up_camera_ = fb_camera_;
            return stop("the stick turns nothing: the game doesn't read the controller through a path Gyro4All can "
                        "change (until the camera changes)");
        }
    }
    ++frames_;
    probe_ = !forced_ && frames_ % kProbeFrames == 0;
}

// Feeds the learner and takes its model once it is ready. An axis stopped at a limit isn't the stick's speed: left out.
// A limit is where the camera stops while pushed toward the furthest it has been that way since the fallback started
// (a dead zone stops it anywhere, a limit only there; turns into new ground move it).
void StickFallback::learn(double turned_pitch, double turned_yaw, bool usable) {
    float x, y;
    learner_.given_at(model_.lag, x, y);
    double expected_yaw, expected_pitch;
    model_.speeds(x, y, expected_yaw, expected_pitch);
    const auto at_limit = [&](Reach& r, double expected, double turned) {
        r.at += turned;
        const bool pushed = expected > 0 ? r.at >= r.high - kLimitMargin : expected < 0 && r.at <= r.low + kLimitMargin;
        const bool stopped = std::abs(turned) <= 0.1 * std::abs(expected) * dt_;
        r.high = std::max(r.high, r.at);
        r.low = std::min(r.low, r.at);
        return pushed && stopped;
    };
    seen_yaw_ = turned_yaw;
    seen_pitch_ = turned_pitch;
    used_yaw_ = !at_limit(reach_yaw_, expected_yaw, turned_yaw) && usable;
    used_pitch_ = !at_limit(reach_pitch_, expected_pitch, turned_pitch) && usable;
    learner_.seen(turned_yaw, turned_pitch, used_yaw_, used_pitch_);
    if (StickModel m; learner_.model(prior_, m)) {
        model_ = m;
        learnt_ = true;
    }
    if (log_wait_ > 0) --log_wait_;
    if (save_wait_ > 0) --save_wait_;
    if (learnt_ && log_wait_ == 0 && (!logged_once_ || differs(model_, logged_, 0.1))) {
        log("stick fallback: the game's stick, learnt: %s (the model explains %.0f%% of the camera's motion)",
            model_.describe().c_str(), learner_.fit() * 100);
        logged_ = model_;
        logged_once_ = true;
        log_wait_ = kLogFrames;
    }
}

void StickFallback::turn(double pitch, double yaw, bool written) {
    // The stick carries the gyro's turn on a probe frame too (steady for the probe's verdict): the game undoes the
    // write, or keeps it and the fallback ends next frame (that one frame turned twice).
    if (on_) {
        gyro_pitch_ += pitch;
        gyro_yaw_ += yaw;
    }
    if (written && known_) write_ = {true, camera_, pitch_, yaw_, pitch, yaw};
}

bool StickFallback::stick(float& x, float& y) {
    if (!on_) return false;
    const bool still = gyro_yaw_ == 0 && gyro_pitch_ == 0;
    rest_yaw_ = std::clamp(rest_yaw_ + gyro_yaw_, -kMaxRest, kMaxRest);
    rest_pitch_ = std::clamp(rest_pitch_ + gyro_pitch_, -kMaxRest, kMaxRest);
    gyro_yaw_ = gyro_pitch_ = 0;
    // The speeds wanted, as shares of the game's top speed (signed by its sense). Below the smallest share a
    // deflection past the dead zone gives: nothing while it builds up, then a pulse of that share; a still gyro drops
    // what is left under half of it.
    const StickModel& m = model_;
    const double top_yaw = std::abs(m.yaw_speed) < 5 ? std::copysign(5.0, m.yaw_speed) : m.yaw_speed;
    const double top_pitch = std::abs(m.pitch_speed) < 5 ? std::copysign(5.0, m.pitch_speed) : m.pitch_speed;
    double ux = rest_yaw_ / dt_ / top_yaw, uy = rest_pitch_ / dt_ / top_pitch;
    const double least = m.unit(std::min(1.0, std::clamp(m.deadzone, 0.0, 0.9) + kPulse));
    if (m.radial) {
        const double r = std::hypot(ux, uy);
        if (r > 0 && r < least) {
            if (r < 0.5 * least) {
                ux = uy = 0;
                if (still) rest_yaw_ = rest_pitch_ = 0;
            } else {
                ux *= least / r;
                uy *= least / r;
            }
        }
    } else {
        const auto axis = [&](double& u, double& rest) {
            if (const double a = std::abs(u); a > 0 && a < least) {
                if (a < 0.5 * least) {
                    u = 0;
                    if (still) rest = 0;
                } else {
                    u = std::copysign(least, u);
                }
            }
        };
        axis(ux, rest_yaw_);
        axis(uy, rest_pitch_);
    }
    m.deflection(ux * top_yaw, uy * top_pitch, x, y);
    double given_yaw, given_pitch;
    m.speeds(x, y, given_yaw, given_pitch);
    rest_yaw_ -= given_yaw * dt_;
    rest_pitch_ -= given_pitch * dt_;
    stick_x_ = x;
    stick_y_ = y;
    learner_.given(x, y, dt_);
    return true;
}

void StickFallback::force(bool on) {
    if (on == forced_) return;
    forced_ = on;
    if (!on && on_) stop("StickFallbackTest interval over");
}

std::string StickFallback::status() const {
    if (!on_) return {};
    const std::string source = blind_      ? std::string(has_saved_ ? "saved for this game, blind" : "default, blind")
                               : learnt_   ? std::format("learnt, explains {:.0f}%", learner_.fit() * 100)
                               : has_saved_ ? std::string("saved for this game, learning")
                                            : std::string("default, learning");
    return std::format("stick fallback{}: stick ({:.2f}, {:.2f}), game's stick: {} ({})", forced_ ? " (forced)" : "",
                       stick_x_, stick_y_, model_.describe(), source);
}

bool StickFallback::take_save(std::string& text) {
    // Only a model that explains the camera well, from enough data, is kept for the next session.
    if (!learnt_ || save_wait_ > 0 || learner_.fit() < kSaveFit || learner_.windows() < kSaveWindows ||
        (has_saved_ && !differs(model_, saved_model_, 0.05)))
        return false;
    text = model_.serialize();
    saved_model_ = model_;
    has_saved_ = true;
    save_wait_ = kSaveFrames;
    return true;
}

std::string StickFallback::trace() const {
    if (!on_) return {};
    return std::format("K,{:.4f},{:.4f},{:.5f},{:.5f},{}{}", stick_x_, stick_y_, seen_yaw_, seen_pitch_, used_yaw_ ? 1 : 0,
                       used_pitch_ ? 1 : 0);
}

void StickFallback::note_write(bool undone) {
    if (!undone) {
        undone_run_ = 0;
        if (on_ && !forced_) stop("the game keeps the gyro's writes again");
        return;
    }
    if (on_ || camera_ == gave_up_camera_) return;
    if (++undone_run_ >= kUndoneFrames) start(nullptr);
}

void StickFallback::start(const char* why) {
    on_ = true;
    probe_ = false;
    frames_ = 0;
    fb_camera_ = camera_;
    last_pitch_ = pitch_;
    last_yaw_ = yaw_;
    last_ns_ = 0;
    gyro_pitch_ = gyro_yaw_ = rest_pitch_ = rest_yaw_ = 0;
    last_turn_pitch_ = last_turn_yaw_ = 0;
    reach_pitch_ = reach_yaw_ = {};
    stick_x_ = stick_y_ = 0;
    effect_frames_ = 0;
    effect_turned_ = 0;
    effect_seen_ = false;
    quiet_ = 0;
    undone_run_ = 0;
    learner_.restart();
    const std::string model = model_.describe() + (learnt_ ? " (learnt)" : has_saved_ ? " (saved)" : " (default, to learn)");
    if (why) log("stick fallback: %s: gyro as right-stick input; game's stick: %s", why, model.c_str());
    else log("stick fallback: the game undid the gyro's camera writes %d frames in a row: gyro as right-stick input; "
             "game's stick: %s", kUndoneFrames, model.c_str());
}

void StickFallback::stop(const char* why) {
    on_ = false;
    probe_ = false;
    stick_x_ = stick_y_ = 0;
    rest_pitch_ = rest_yaw_ = 0;
    undone_run_ = 0;
    save_wait_ = 0;
    log("stick fallback off: %s", why);
}

}  // namespace gyro4all
