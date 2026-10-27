#pragma once
// The game's own right stick, seen from outside: how fast the camera turns for each deflection. The stick fallback
// (stick_fallback.hpp) gives the game the deflection that, by this model, turns the camera at the gyro's speed, and
// identifies the model from what the camera does with the deflection it gives. Engine-neutral, no game memory access,
// unit-tested.
//
// Model: speed = top speed (per axis, signed: negative = the game turns that axis the other way) x shape(deflection),
// seen `lag` frames after the deflection is given. Shape: the deflection past a dead zone, rescaled to 0..1 and raised to
// a curve exponent; axial (each axis on its own) or radial (the stick's length, direction kept). This covers what games
// usually do (UE's axial dead zone and exponent, id Tech's radial dead zone and power curve, Steam's own output).
//
// Identification (StickLearner): for every shape of a grid (axial/radial x dead zone 0..0.35 x curve 1..3) and lag
// (0..kMaxLag frames), a least-squares fit of the camera's turn over a short window (kWindow frames: uneven frame
// timing, the game's smoothing) against the shape's turn at full speed 1 over the same window, kLag frames earlier. Its
// slope is the top speed of that axis; the shape and lag with the smallest residual win. Each window is weighted by
// relative error (the chosen model's turn for it: slow turns and the dead zone count as much as fast turns, which
// matters most for aiming) and by how steady the deflection was (a game smoothing the stick before its curve turns
// differently while it changes). The sums fade (half-life about 20 s of data), so the model follows a sensitivity
// changed in the game's options. Only the camera's turn the stick alone made is fed in (stick_fallback.cpp leaves out
// the player's stick, direct writes, jumps, limits). About 9 us per frame, only while the fallback runs.
#include <string>
#include <vector>

namespace gyro4all {

struct StickModel {
    bool radial = true;
    double deadzone = 0.20;      // 0..0.9 of the stick
    double curve = 1.5;          // exponent; 1 = linear
    double yaw_speed = 160;      // degrees/s at full deflection, signed (yaw right for a stick right)
    double pitch_speed = 110;    // (pitch up for a stick up)
    int lag = 2;                 // frames

    // The share of the top speed for a deflection of length m (0..1), and back.
    double unit(double m) const;
    double inverse_unit(double u) const;
    // The camera's speed (degrees/s, GyroLib's sense) for a deflection (x right, y up).
    void speeds(float x, float y, double& yaw, double& pitch) const;
    // The deflection that turns the camera at these speeds (no anti-dead zone below min_unit: see StickFallback::stick).
    void deflection(double yaw, double pitch, float& x, float& y) const;
    // Text form for the profile (StickFallbackLearned=) and the log.
    std::string serialize() const;
    bool parse(const std::string& text);
    std::string describe() const;
};

class StickLearner {
public:
    static constexpr int kDeadzones = 15;  // 0, 0.025 .. 0.35
    static constexpr int kCurves = 9;      // 1, 1.25 .. 3
    static constexpr int kShapes = 2 * kDeadzones * kCurves;
    static constexpr int kMaxLag = 10;
    static constexpr int kWindow = 8;
    static constexpr int kRing = kWindow + kMaxLag + 2;
    static constexpr double kFade = 0.99942;  // per window used: half-life about 1200 windows (20 s at 60 frames/s)
    static constexpr double kMinWindows = 90;  // windows with a clear deflection before a model is used (yaw + pitch)
    static constexpr double kMinAxisWindows = 45;  // ... before an axis's own top speed is used
    static constexpr double kMinFit = 0.5;     // share of the camera's motion the model must explain

    StickLearner();
    // The fallback (re)starts: the deflection history is cleared, the sums are kept (same game).
    void restart();
    // The deflection given this frame (x right, y up), held for dt seconds.
    void given(float x, float y, double dt);
    // The camera's turn since the last frame (degrees, GyroLib's sense). use_*: that axis's turn is the stick's alone.
    void seen(double yaw, double pitch, bool use_yaw, bool use_pitch);
    // The deflection given `frames_ago` frames before the last one (0 = the last; 0 beyond the history).
    void given_at(int frames_ago, float& x, float& y) const;
    // The fitted model, if there is enough data and it explains the camera well; the axis without enough data of its
    // own keeps `prior`'s top speed scaled like the other axis's.
    bool model(const StickModel& prior, StickModel& out) const;
    double fit() const { return best_fit_; }            // 0..1, of the chosen shape and lag
    double windows() const { return n_[0] + n_[1]; }    // weight of the data behind it

private:
    static StickModel shape_model(int shape, int lag);
    void choose();

    // Per frame in the ring: the deflection, and the running sums of each shape's unit turn (x then y).
    struct Frame {
        float x = 0, y = 0;
    };
    Frame frames_[kRing];
    std::vector<double> cum_;  // [kRing][2][kShapes]
    long long count_ = 0;      // frames given since restart
    // The camera's turn over the last kWindow frames, and how many frames in a row were usable, per axis.
    double turn_[2][kWindow] = {};
    int usable_[2] = {};
    int seen_count_ = 0;
    // Weighted fit sums per axis, lag, shape: of V * P and of P * P; per axis: of V * V, and the windows with a clear
    // deflection (unweighted: how much data there is).
    std::vector<double> vp_, pp_;
    double vv_[2] = {}, n_[2] = {};
    int best_lag_ = -1, best_shape_ = -1;
    double best_fit_ = 0;
};

}  // namespace gyro4all
