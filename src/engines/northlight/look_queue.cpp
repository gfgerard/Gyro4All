// What Northlight's camera hooks share (look_queue.hpp). The queue's timing comes from Control (MODLOG 2026-10-30): the
// game thread's frames come about 1.8 times per camera update and out of step with it, so taking "what was added" still
// gave each update one or two frames' worth (steps p10 0.46, p90 2.09 x the median); the update jobs start at uneven
// times (5-10 ms apart at about 135/s) while the game moves the camera by an even step per update (the stick's steps
// varied 2.4% from update to update, its rate per real time 10%), and taken at the update's real time the gyro's steps
// followed that scheduling noise (20%). Alan Wake 2 (MODLOG 2026-10-30): its camera runs on a fixed 30 Hz step (dt
// 33.3 ms) inside ~68 fps rendering, so its updates come 29 or 44 ms apart, and Gyro4All's frames come once per Present
// (~15 ms apart): read 6 ms behind real time the totals were often past the newest frame and extrapolated from about one
// frame's rate, so a step came short and the next one doubled (3-5 per 5 s).
#include "look_queue.hpp"

#include "mem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gyro4all::northlight {

namespace {

constexpr double kPi = 3.14159265358979323846, kDeg = 180.0 / kPi;

int64_t qpc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

}  // namespace

Section exe_section(const char* name) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0)
            return {base + s->VirtualAddress, base + s->VirtualAddress + std::min(s->Misc.VirtualSize, s->SizeOfRawData)};
    return {};
}

std::vector<uint8_t> copy(const Section& s) {
    std::vector<uint8_t> out(s.end - s.begin);
    if (out.empty() || !mem::guarded_copy(out.data(), s.begin, out.size())) out.clear();
    return out;
}

uintptr_t function_start(uintptr_t pc) {
    DWORD64 image = 0;
    const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(pc, &image, nullptr);
    for (int i = 0; f && i < 8; ++i) {
        const auto* info = reinterpret_cast<const uint8_t*>(image + f->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO)) return uintptr_t(image + f->BeginAddress);
        const size_t codes = (info[2] + 1) & ~1u;  // CountOfCodes, rounded up to even
        f = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
    }
    return 0;
}

uintptr_t find_unique_code(const uint8_t* pattern, size_t size, size_t any_at, int& hits) {
    const Section text = exe_section(".text");
    const std::vector<uint8_t> t = copy(text);
    uintptr_t at = 0;
    hits = 0;
    for (size_t i = 0; i + size <= t.size(); ++i) {
        if (t[i] != pattern[0]) continue;
        bool match = true;
        for (size_t j = 1; j < size && match; ++j) match = j == any_at || t[i + j] == pattern[j];
        if (match) at = text.begin + i, ++hits;
    }
    return hits == 1 ? at : 0;
}

bool read3(uintptr_t a, double v[3]) {
    float f[3];
    if (!mem::guarded_copy(f, a, sizeof f)) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(f[i])) return false;
        v[i] = f[i];
    }
    return true;
}

double length(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

bool look_angles(uintptr_t a, double& pitch, double& heading) {
    double v[3];
    if (!read3(a, v)) return false;
    const double len = length(v);
    if (len < 0.5) return false;
    pitch = std::asin(std::clamp(v[1] / len, -1.0, 1.0)) * kDeg;
    heading = std::atan2(v[0], v[2]) * kDeg;
    return true;
}

bool turn_look(uintptr_t a, double pitch, double yaw, double max_up, double max_down) {
    double v[3];
    if (!read3(a, v)) return false;
    const double len = length(v);
    if (len < 0.5) return false;
    const double p = std::clamp(std::asin(std::clamp(v[1] / len, -1.0, 1.0)) * kDeg + pitch, -max_down, max_up) / kDeg;
    const double h = std::atan2(v[0], v[2]) + yaw / kDeg;
    const float out[3] = {float(len * std::cos(p) * std::sin(h)), float(len * std::sin(p)), float(len * std::cos(p) * std::cos(h))};
    return mem::guarded_store(a, out, sizeof out);
}

// The totals at time t: between two frames, interpolated; past the newest, the newest plus the rate over the last ~10 ms
// of frames for the time since it, up to 20 ms; before the oldest kept, the oldest.
LookQueue::Total LookQueue::totals_at(int64_t t) const {
    const Total& last = totals_[last_];
    Total out = last;
    if (!freq_ || !last.t) return out;
    if (t < last.t) {
        for (int i = 1; i < 16; ++i) {
            const Total& older = totals_[(last_ - i + 16) % 16];
            const Total& newer = totals_[(last_ - i + 17) % 16];
            if (!older.t || older.t >= newer.t) return newer;
            if (older.t <= t) {
                const double f = double(t - older.t) / double(newer.t - older.t);
                return {t, older.pitch + (newer.pitch - older.pitch) * f, older.yaw + (newer.yaw - older.yaw) * f};
            }
        }
        return totals_[(last_ + 1) % 16].t ? totals_[(last_ + 1) % 16] : out;
    }
    const Total* from = nullptr;
    for (int i = 1; i < 16; ++i) {
        const Total& e = totals_[(last_ - i + 16) % 16];
        if (!e.t || e.t >= last.t) break;
        from = &e;
        if (last.t - e.t >= freq_ / 100) break;  // 10 ms
    }
    if (!from) return out;
    const double span = double(last.t - from->t) / double(freq_);
    const double ahead = std::clamp(double(t - last.t) / double(freq_), 0.0, 0.020);
    out.pitch += (last.pitch - from->pitch) / span * ahead;
    out.yaw += (last.yaw - from->yaw) / span * ahead;
    return out;
}

// The camera update's clock (QueryPerformanceCounter units): it advances by the game's frame time dt (seconds) when
// known, else by the average update interval, pulled gently toward real time minus the delay, never backwards. The delay
// is more than Gyro4All's frames' spacing (6 ms, or 1.5 times that spacing), so the clock reads between two of them.
int64_t LookQueue::frame_clock(double dt_game) {
    const int64_t now = qpc(), delay = std::max(freq_ * 6 / 1000, int64_t(add_spacing_ * 1.5));
    const int64_t target = now - delay;
    if (!last_call_ || now - last_call_ > freq_ / 10) {  // first update, or one after a pause or loading: start over
        clock_ = target;
        interval_ = 0;
    } else {
        const double dt = double(now - last_call_);
        interval_ = interval_ > 0 ? interval_ * 0.9 + dt * 0.1 : dt;
        const int64_t before = clock_;
        clock_ += dt_game > 0 && dt_game < 0.1 ? int64_t(dt_game * double(freq_)) : int64_t(interval_);
        const int64_t error = target - clock_;
        if (error > freq_ / 20 || error < -freq_ / 20) clock_ = target;  // a hitch (50 ms off): start over
        else clock_ += error / 10;
        clock_ = std::max(clock_, before);  // never back: that would turn the camera back a little
        clock_ = std::min(clock_, now);
    }
    last_call_ = now;
    return clock_;
}

void LookQueue::add(double pitch, double yaw, double max_up, double max_down) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(mutex_);
    if (!freq_) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq_ = f.QuadPart;
    }
    Total next = totals_[last_];
    // Untaken for 100 ms (pause, loading): what is left is dropped, and the rate starts over.
    if (now - last_take_.load() > 100) {
        for (Total& e : totals_) e = {};
        applied_pitch_ = next.pitch, applied_yaw_ = next.yaw;
    }
    next.t = qpc();
    if (const int64_t gap = next.t - totals_[last_].t; totals_[last_].t && gap > 0 && gap < freq_ / 10)
        add_spacing_ = add_spacing_ > 0 ? add_spacing_ * 0.95 + double(gap) * 0.05 : double(gap);
    next.pitch += pitch;
    next.yaw += yaw;
    last_ = (last_ + 1) % 16;
    totals_[last_] = next;
    max_up_ = max_up;
    max_down_ = max_down;
}

bool LookQueue::take(double& pitch, double& yaw, double& max_up, double& max_down, double dt) {
    {
        std::lock_guard lock(mutex_);
        const Total now = freq_ ? totals_at(frame_clock(dt)) : totals_[last_];
        pitch = now.pitch - applied_pitch_, yaw = now.yaw - applied_yaw_, max_up = max_up_, max_down = max_down_;
        applied_pitch_ = now.pitch, applied_yaw_ = now.yaw;
    }
    last_take_ = GetTickCount64();
    return pitch != 0 || yaw != 0;
}

double LookQueue::pending_pitch() {
    std::lock_guard lock(mutex_);
    return totals_[last_].pitch - applied_pitch_;
}

}  // namespace gyro4all::northlight
