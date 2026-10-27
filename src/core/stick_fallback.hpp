#pragma once
// Stick fallback: when the game undoes the gyro's camera writes frame after frame (it rebuilds the angles from state of
// its own, e.g. RE3's scripted aim), the gyro's turn goes to the game as right-stick deflection instead. Engine-neutral,
// no game memory access: host.cpp drives it once per frame and passes the stick to every controller path Gyro4All owns
// (the game's DirectInput controllers, the XInput controller it shows or a real one, the game's own PlayStation reports).
//
// - Detection: a direct write counts as undone when, next frame, the angles (Engine::view_angles) are within half the
//   step of where the camera would be without it, kept when within half the step of where it would be with it (steps
//   over 0.11 degrees only; no verdict while the player's own stick moves). kUndoneFrames undone writes in a row start
//   the fallback (judged one frame after the last).
// - Output: open loop, like Steam Input's gyro to joystick camera: each frame, the deflection that turns the camera at
//   the gyro's speed by the model of the game's stick (stick_response.hpp: dead zone, curve, top speed and sense per
//   axis). The camera's angles are never fed back into the deflection, so a still gyro is a centred stick: no drift.
//   A turn slower than the smallest deflection past the dead zone builds up (at most kMaxRest degrees, with the turn
//   past the game's top speed) and goes out as short pulses, so slow aiming isn't lost.
// - Model: identified automatically from what the camera does with the deflection given (StickLearner), using only
//   frames where the stick alone turned it: not the player's own stick (and kMaxLag frames after), direct writes,
//   jumps over kJump degrees, or an axis stopped at a limit (pushed toward the furthest the camera has been that way,
//   and it doesn't move).
//   Until it has enough data: the model saved for this game (StickFallbackLearned= in its profile), else a default.
//   The learnt model is saved when it changes (take_save).
// - End: a direct probe write every kProbeFrames frames that lasts, another camera, or no readable view. A stick that
//   turns nothing (the game doesn't read the controller through a path Gyro4All can change) gives up for that camera.
// - Forced (StickFallbackTest): on whatever the writes do, without probe writes, for testing it in any game.
// - Blind (frame_blind, EngineFrame::no_camera): the engine has no hook for the game's camera, so the gyro always goes
//   to the stick, with the saved model (else the default): no angles, so nothing to detect, learn or probe.
//
// Dead ends (MODLOG 2026-10-17..19): a closed loop on the camera's angles that owed the turn the camera hadn't made and
// learnt the curve and sense while controlling (runaways, false sense flips, a curve learnt at the pitch limit, drift);
// a fixed open-loop model (TNO's stick is slower and curved: the gyro hardly turned the camera).
#include "stick_response.hpp"

#include <cstdint>
#include <string>

namespace gyro4all {

class StickFallback {
public:
    // The model saved for this game (StickFallbackLearned=): used until the learnt one is ready. False if unreadable.
    bool restore(const std::string& text);

    // Start of the frame, before the turn. known: the engine read the active view's angles (degrees, GyroLib's sense:
    // pitch up, yaw right); camera: which camera they belong to; player_stick: the player's own right stick is
    // deflected; now_ns: a monotonic clock.
    void frame(bool known, double pitch, double yaw, uintptr_t camera, bool player_stick, uint64_t now_ns);
    // Instead of frame() while the engine has no hook for the game's camera (EngineFrame::no_camera): the fallback runs
    // blind (on = in gameplay: focus, no pause or cursor, no panel; off otherwise).
    void frame_blind(bool on, uint64_t now_ns);
    bool blind() const { return blind_; }
    // Whether this frame's turn goes to the camera (false while the fallback gives it to the stick, except probe frames).
    bool direct() const { return !on_ || probe_; }
    // This frame's gyro turn (degrees, GyroLib's sense) after direct(): written = the engine wrote it to the camera.
    void turn(double pitch, double yaw, bool written);
    // The right stick for the game this frame (x right, y up, -1..1). False = none (0, 0).
    bool stick(float& x, float& y);
    bool active() const { return on_; }
    // Forced on (from the next frame with a readable view) or back to detection.
    void force(bool on);
    // The status line part, "" when off.
    std::string status() const;
    // A learnt model to save to the game's profile (StickFallbackLearned=), once per change worth saving.
    bool take_save(std::string& text);
    // For MotionTrace: this frame's deflection, the camera's turn and what the learner took ("" when off).
    std::string trace() const;
    const StickModel& model() const { return model_; }
    bool learnt() const { return learnt_; }

    static constexpr int kUndoneFrames = 8;    // undone writes in a row that start the fallback
    static constexpr int kProbeFrames = 90;    // a direct write every this many frames in the fallback
    static constexpr int kEffectFrames = 45;   // frames of clear deflection after which a stick that turned nothing gives up
    static constexpr double kMaxRest = 2.0;    // degrees of the gyro's turn not given yet, at most
    static constexpr double kPulse = 0.04;     // the smallest deflection given: this far past the model's dead zone
    static constexpr double kJump = 30;        // degrees in a frame: not a stick's turn (a cut, a respawn)
    static constexpr double kLimitMargin = 0.5;  // degrees from the furthest reached that still count as there

private:
    void note_write(bool undone);
    void start(const char* why);
    void stop(const char* why);
    void learn(double turned_pitch, double turned_yaw, bool usable);

    // The angles this frame (from frame()).
    bool known_ = false;
    double pitch_ = 0, yaw_ = 0;
    uintptr_t camera_ = 0;
    // Last frame's direct write.
    struct Write {
        bool valid = false;
        uintptr_t camera = 0;
        double pre_pitch = 0, pre_yaw = 0;  // the angles before it
        double pitch = 0, yaw = 0;          // the gyro's turn in it
    } write_;
    int undone_run_ = 0;
    uintptr_t gave_up_camera_ = 0;  // the stick turned nothing on this camera

    // The fallback.
    bool on_ = false;
    bool forced_ = false;
    bool blind_ = false;  // running blind (frame_blind)
    bool probe_ = false;
    int frames_ = 0;
    uintptr_t fb_camera_ = 0;
    double last_pitch_ = 0, last_yaw_ = 0;  // the camera's angles last frame
    uint64_t last_ns_ = 0;
    double dt_ = 1.0 / 60;
    double gyro_pitch_ = 0, gyro_yaw_ = 0;  // the gyro's turn this frame
    double rest_pitch_ = 0, rest_yaw_ = 0;  // the gyro's turn not given yet (degrees)
    double last_turn_pitch_ = 0, last_turn_yaw_ = 0;  // the camera's turn last frame
    // How far the camera has turned since the fallback started, and the furthest each way (limits).
    struct Reach {
        double at = 0, low = 0, high = 0;
    } reach_pitch_, reach_yaw_;
    float stick_x_ = 0, stick_y_ = 0;       // deflection given last frame
    int effect_frames_ = 0;                 // frames of clear deflection without the player's stick
    double effect_turned_ = 0;              // degrees the camera turned in them
    bool effect_seen_ = false;              // the stick turns the camera
    int quiet_ = 0;                         // frames left in which the camera's turn isn't the stick's alone
    // This frame's observation (trace).
    double seen_pitch_ = 0, seen_yaw_ = 0;
    bool used_pitch_ = false, used_yaw_ = false;

    // The game's stick.
    StickModel prior_;     // saved for this game, else the default
    StickModel model_;     // in use
    bool learnt_ = false;  // model_ is the learner's
    StickLearner learner_;
    StickModel logged_;  // last model logged
    bool logged_once_ = false;
    int log_wait_ = 0;
    std::string saved_;  // last saved (or restored) text
    StickModel saved_model_;
    bool has_saved_ = false;
    int save_wait_ = 0;
};

}  // namespace gyro4all
