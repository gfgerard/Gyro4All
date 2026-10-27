#include "stick_response.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace gyro4all {

namespace {

constexpr double kDeadzoneStep = 0.025, kCurveStep = 0.25;
constexpr double kMinSpeed = 5;      // degrees/s: a fitted top speed is kept at least this far from 0
constexpr double kMinTopSpeed = 20;  // degrees/s at full stick: a slower fit isn't a game's stick
constexpr float kClear = 0.3f;       // a deflection this large counts as data for that axis
constexpr float kSome = 0.02f;       // under this on an axis, a window says nothing about it
constexpr double kSwitch = 0.97;     // another shape or lag wins only with a residual this much smaller
constexpr double kSlowTurn = 0.5;    // degrees per window (about 4 deg/s): turns under it are weighted alike
constexpr double kSteadyRange = 0.08;  // deflection range over a window's span that halves its weight

double floor_speed(double s) { return std::abs(s) < kMinSpeed ? std::copysign(kMinSpeed, s) : s; }

}  // namespace

double StickModel::unit(double m) const {
    const double dz = std::clamp(deadzone, 0.0, 0.9);
    if (m <= dz) return 0;
    return std::pow(std::min(1.0, (m - dz) / (1 - dz)), curve);
}

double StickModel::inverse_unit(double u) const {
    if (u <= 0) return 0;
    const double dz = std::clamp(deadzone, 0.0, 0.9);
    return dz + (1 - dz) * std::pow(std::min(u, 1.0), 1 / std::max(curve, 0.1));
}

void StickModel::speeds(float x, float y, double& yaw, double& pitch) const {
    if (radial) {
        const double m = std::hypot(double(x), double(y));
        const double g = m > 0 ? unit(m) / m : 0;
        yaw = yaw_speed * g * x;
        pitch = pitch_speed * g * y;
    } else {
        yaw = yaw_speed * std::copysign(unit(std::abs(x)), x);
        pitch = pitch_speed * std::copysign(unit(std::abs(y)), y);
    }
}

void StickModel::deflection(double yaw, double pitch, float& x, float& y) const {
    const double ux = yaw / floor_speed(yaw_speed), uy = pitch / floor_speed(pitch_speed);
    if (radial) {
        const double r = std::hypot(ux, uy);
        const double k = r > 0 ? inverse_unit(r) / r : 0;
        x = float(std::clamp(ux * k, -1.0, 1.0));
        y = float(std::clamp(uy * k, -1.0, 1.0));
    } else {
        x = float(std::copysign(inverse_unit(std::abs(ux)), ux));
        y = float(std::copysign(inverse_unit(std::abs(uy)), uy));
    }
}

std::string StickModel::serialize() const {
    return std::format("{},{:.3f},{:.2f},{:.1f},{:.1f},{}", radial ? "radial" : "axial", deadzone, curve, yaw_speed,
                       pitch_speed, lag);
}

bool StickModel::parse(const std::string& text) {
    StickModel m;
    const char* p = text.data();
    const char* end = p + text.size();
    const auto skip = [&] {
        while (p < end && (*p == ' ' || *p == ',')) ++p;
    };
    skip();
    if (text.compare(size_t(p - text.data()), 6, "radial") == 0) m.radial = true, p += 6;
    else if (text.compare(size_t(p - text.data()), 5, "axial") == 0) m.radial = false, p += 5;
    else return false;
    double* numbers[] = {&m.deadzone, &m.curve, &m.yaw_speed, &m.pitch_speed};
    for (double* n : numbers) {
        skip();
        const auto r = std::from_chars(p, end, *n);
        if (r.ec != std::errc() || !std::isfinite(*n)) return false;
        p = r.ptr;
    }
    skip();
    const auto r = std::from_chars(p, end, m.lag);
    if (r.ec != std::errc()) return false;
    if (m.deadzone < 0 || m.deadzone > 0.9 || m.curve < 0.2 || m.curve > 5 || std::abs(m.yaw_speed) < kMinTopSpeed ||
        std::abs(m.pitch_speed) < kMinTopSpeed || m.lag < 0 || m.lag > StickLearner::kMaxLag)
        return false;
    *this = m;
    return true;
}

std::string StickModel::describe() const {
    return std::format("{} dead zone {:.2f}, curve {:.2f}, yaw {:.0f}{} and pitch {:.0f}{} deg/s at full stick, {} frame{} "
                       "late",
                       radial ? "radial" : "axial", deadzone, curve, std::abs(yaw_speed), yaw_speed < 0 ? " (inverted)" : "",
                       std::abs(pitch_speed), pitch_speed < 0 ? " (inverted)" : "", lag, lag == 1 ? "" : "s");
}

StickLearner::StickLearner()
    : cum_(size_t(kRing) * 2 * kShapes), vp_(size_t(2) * (kMaxLag + 1) * kShapes), pp_(vp_.size()) {}

StickModel StickLearner::shape_model(int shape, int lag) {
    StickModel m;
    m.radial = shape >= kDeadzones * kCurves;
    const int rest = shape % (kDeadzones * kCurves);
    m.deadzone = (rest / kCurves) * kDeadzoneStep;
    m.curve = 1 + (rest % kCurves) * kCurveStep;
    m.lag = lag;
    return m;
}

void StickLearner::restart() {
    count_ = 0;
    seen_count_ = 0;
    usable_[0] = usable_[1] = 0;
    for (auto& axis : turn_) std::fill(std::begin(axis), std::end(axis), 0.0);
}

void StickLearner::given(float x, float y, double dt) {
    const int slot = int(count_ % kRing);
    const int prev = int((count_ + kRing - 1) % kRing);
    frames_[slot] = {x, y};
    double* now = &cum_[size_t(slot) * 2 * kShapes];
    const double* before = &cum_[size_t(prev) * 2 * kShapes];
    const double ax = std::abs(double(x)), ay = std::abs(double(y)), m = std::hypot(ax, ay);
    for (int dzi = 0; dzi < kDeadzones; ++dzi) {
        const double dz = dzi * kDeadzoneStep;
        // The share past the dead zone, before the curve: per axis, and of the length.
        const double sx = ax > dz ? std::min(1.0, (ax - dz) / (1 - dz)) : 0;
        const double sy = ay > dz ? std::min(1.0, (ay - dz) / (1 - dz)) : 0;
        const double sm = m > dz ? std::min(1.0, (m - dz) / (1 - dz)) : 0;
        for (int ci = 0; ci < kCurves; ++ci) {
            const double curve = 1 + ci * kCurveStep;
            const int axial = dzi * kCurves + ci, radial = kDeadzones * kCurves + axial;
            const double px = sx > 0 ? std::copysign(std::pow(sx, curve), x) : 0;
            const double py = sy > 0 ? std::copysign(std::pow(sy, curve), y) : 0;
            const double g = sm > 0 ? std::pow(sm, curve) / m : 0;
            const double add[2][2] = {{px, py}, {g * x, g * y}};
            for (int type = 0; type < 2; ++type) {
                const int s = type ? radial : axial;
                for (int a = 0; a < 2; ++a) {
                    const double base = count_ ? before[a * kShapes + s] : 0;
                    now[a * kShapes + s] = base + add[type][a] * dt;
                }
            }
        }
    }
    ++count_;
}

void StickLearner::given_at(int frames_ago, float& x, float& y) const {
    x = y = 0;
    if (frames_ago < 0 || frames_ago >= kRing || frames_ago >= count_) return;
    const Frame& f = frames_[(count_ - 1 - frames_ago) % kRing];
    x = f.x;
    y = f.y;
}

void StickLearner::seen(double yaw, double pitch, bool use_yaw, bool use_pitch) {
    const double turns[2] = {yaw, pitch};
    const bool use[2] = {use_yaw, use_pitch};
    const int at = seen_count_ % kWindow;
    ++seen_count_;
    for (int a = 0; a < 2; ++a) {
        turn_[a][at] = turns[a];
        usable_[a] = use[a] ? usable_[a] + 1 : 0;
    }
    // Every lag needs its whole window of deflection: nothing until the history is that long.
    if (count_ < kMaxLag + kWindow + 1) return;
    const long long latest = count_ - 1;
    bool updated = false;
    for (int a = 0; a < 2; ++a) {
        if (usable_[a] < kWindow) continue;
        // Deflection on this axis anywhere a window of some lag can see it.
        float most = 0;
        for (int i = 0; i < kMaxLag + kWindow; ++i) {
            const Frame& f = frames_[(latest - i) % kRing];
            most = std::max(most, std::abs(a ? f.y : f.x));
        }
        if (most < kSome) continue;
        double v = 0;
        for (double t : turn_[a]) v += t;
        // Weight: relative error (by the chosen model's turn for this window, so slow turns count as much as fast
        // ones, and the dead zone most), and a steady deflection (a game smoothing the stick before its curve turns
        // differently while the deflection changes).
        float low = 2, high = -2;
        for (int i = 0; i < kMaxLag + kWindow; ++i) {
            const Frame& f = frames_[(latest - i) % kRing];
            low = std::min(low, a ? f.y : f.x);
            high = std::max(high, a ? f.y : f.x);
        }
        const double range = (high - low) / kSteadyRange;
        double w = 1 / (1 + range * range);
        if (best_shape_ >= 0) {
            const size_t i = (size_t(a) * (kMaxLag + 1) + best_lag_) * kShapes + best_shape_;
            const double* hi = &cum_[size_t((latest - best_lag_) % kRing) * 2 * kShapes + size_t(a) * kShapes];
            const double* lo = &cum_[size_t((latest - best_lag_ - kWindow) % kRing) * 2 * kShapes + size_t(a) * kShapes];
            const double predicted = pp_[i] > 1e-12 ? vp_[i] / pp_[i] * (hi[best_shape_] - lo[best_shape_]) : 0;
            w /= predicted * predicted + kSlowTurn * kSlowTurn;
        } else {
            w /= v * v + kSlowTurn * kSlowTurn;
        }
        w *= kSlowTurn * kSlowTurn;  // 1 for a slow, steady window
        vv_[a] = kFade * vv_[a] + w * v * v;
        n_[a] = kFade * n_[a] + (most >= kClear ? 1 : 0);
        for (int lag = 0; lag <= kMaxLag; ++lag) {
            const double* hi = &cum_[size_t((latest - lag) % kRing) * 2 * kShapes + size_t(a) * kShapes];
            const double* lo = &cum_[size_t((latest - lag - kWindow) % kRing) * 2 * kShapes + size_t(a) * kShapes];
            double* vp = &vp_[(size_t(a) * (kMaxLag + 1) + lag) * kShapes];
            double* pp = &pp_[(size_t(a) * (kMaxLag + 1) + lag) * kShapes];
            for (int s = 0; s < kShapes; ++s) {
                const double p = hi[s] - lo[s];
                vp[s] = kFade * vp[s] + w * v * p;
                pp[s] = kFade * pp[s] + w * p * p;
            }
        }
        updated = true;
    }
    if (updated) choose();
}

void StickLearner::choose() {
    const double total = vv_[0] + vv_[1];
    if (total <= 0) return;
    const auto residual = [&](int lag, int s) {
        double r = 0;
        for (int a = 0; a < 2; ++a) {
            const size_t i = (size_t(a) * (kMaxLag + 1) + lag) * kShapes + s;
            r += vv_[a] - (pp_[i] > 1e-12 ? vp_[i] * vp_[i] / pp_[i] : 0);
        }
        return r;
    };
    int lag = 0, shape = 0;
    double best = residual(0, 0);
    for (int l = 0; l <= kMaxLag; ++l)
        for (int s = 0; s < kShapes; ++s)
            if (const double r = residual(l, s); r < best) best = r, lag = l, shape = s;
    if (best_shape_ < 0 || best < kSwitch * residual(best_lag_, best_shape_)) best_lag_ = lag, best_shape_ = shape;
    best_fit_ = std::clamp(1 - residual(best_lag_, best_shape_) / total, 0.0, 1.0);
}

bool StickLearner::model(const StickModel& prior, StickModel& out) const {
    if (best_shape_ < 0 || windows() < kMinWindows || best_fit_ < kMinFit) return false;
    StickModel m = shape_model(best_shape_, best_lag_);
    double speed[2] = {};
    bool known[2] = {};
    for (int a = 0; a < 2; ++a) {
        const size_t i = (size_t(a) * (kMaxLag + 1) + best_lag_) * kShapes + best_shape_;
        known[a] = n_[a] >= kMinAxisWindows && pp_[i] > 1e-12;
        if (known[a]) speed[a] = vp_[i] / pp_[i];
        // No game's stick turns this slowly at full deflection: the data isn't the stick's (the game didn't take it).
        if (known[a] && std::abs(speed[a]) < kMinTopSpeed) return false;
    }
    if (!known[0] && !known[1]) return false;
    // An axis without enough data of its own: the prior's speed, scaled like the other axis (its sense kept).
    if (!known[0]) speed[0] = prior.yaw_speed * std::abs(speed[1] / floor_speed(prior.pitch_speed));
    if (!known[1]) speed[1] = prior.pitch_speed * std::abs(speed[0] / floor_speed(prior.yaw_speed));
    m.yaw_speed = floor_speed(speed[0]);
    m.pitch_speed = floor_speed(speed[1]);
    out = m;
    return true;
}

}  // namespace gyro4all
