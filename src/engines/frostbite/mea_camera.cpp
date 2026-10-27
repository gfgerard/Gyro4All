// Mass Effect: Andromeda (MODLOG 2026-10-26). The camera is a chain of ContactCamera (Cinebot) controllers; the stick
// reaches it through ContactCamera::InputControllerAxesStrategy, whose per-frame input read (exe+0xbc44e0 in the EA app
// build) first loads the camera's current yaw / pitch (float degrees, relative to the character, yaw 0..360) into a
// pair in the strategy (+0xC0 / +0xC4), then calls its odometer's vtable slot 3, which adds this frame's stick turn:
//   pair.yaw += rate_x * odometer[+0x10] * sensitivity * dt, pair.pitch += rate_y * odometer[+0x14] * ...
// The camera then follows the pair (with its spring, recentering and limits). Written from outside, the pair is lost to
// the next frame's load; added to right after the odometer, it turns the camera like the stick.
// - Signs (measured): stick right lowers the pair's yaw; looking up raises its pitch. So the gyro's yaw (right
//   positive) is subtracted, its pitch (up positive) added.
// - Found at run time by its code (no exe addresses): the odometer update's prologue (up to `mov rax,[rip+..]`) occurs
//   once in the exe, and its accumulation (`movss xmm0,[rsp+70h]; mulss xmm0,[rdi+10h]` ... `[rdi+14h]`) is checked at
//   its offset. The vtable slots that hold it (in the exe's data) are swapped for the hook: no code is changed.
// - Signature of the slot: (odometer, Vec2 axes by value, Vec2 speeds by value, float dt, float* pair).
// - Whose pair: the player's strategy is the one that runs; each pair the hook sees gets the same total turn and keeps
//   how much of it it already has (a camera blend runs two). The player's update first waits for Gyro4All's frame
//   (sync_gyro_frame), so a frame's turn lands in this update, not the next.
// - Gate: an odometer update in the last 100 ms (the camera takes input).
// - Inverted cameras: the scanner (d-pad down) shows its pair's pitch the other way (the stick still looks up there: the
//   game feeds it inverted), and no flag for it was found (MODLOG). So each pair's relation to the view is learned from
//   whatever turns it, the stick (the odometer's own change) or the gyro: the view is read from the game's static copy
//   of the render view (found by the code that fills it every frame, exe+0x16e78cd), and while the pair's pitch moves,
//   the view's tilt is correlated with it (smoothed: the camera follows the pair through a spring). A pair whose pitch
//   tilts the view down gets the gyro's pitch inverted; until a pair is learned, it is taken as upright. Absolute (view
//   up = up), so neither the order the cameras are used in nor the game's invert-Y option matters.
#include "fb_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace gyro4all::fb {

namespace {

// mov [rsp+18h],r8; mov [rsp+10h],rdx; push rdi; sub rsp,50h; mov rax,[rip+rel32]
constexpr uint8_t kStart[] = {0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24,
                              0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x05};
// movss xmm0,[rsp+70h]; mulss xmm0,[rdi+10h]; mulss xmm0,xmm7 (the yaw's rate times the odometer's yaw scale)
constexpr uint8_t kAccumulate[] = {0xF3, 0x0F, 0x10, 0x44, 0x24, 0x70, 0xF3, 0x0F,
                                   0x59, 0x47, 0x10, 0xF3, 0x0F, 0x59, 0xC7};
constexpr size_t kAccumulateAt = 0xF9;
// mulss xmm0,[rdi+14h]; mulss xmm0,xmm6 (the pitch's)
constexpr uint8_t kAccumulatePitch[] = {0xF3, 0x0F, 0x59, 0x47, 0x14, 0xF3, 0x0F, 0x59, 0xC6};
constexpr size_t kAccumulatePitchAt = 0x120;
constexpr ULONGLONG kRecentMs = 100;
constexpr unsigned kSyncMs = 4;  // longest wait for Gyro4All's frame in the update
// The code that copies the render view to its static copy (exe+0x16e78cd): movups xmm0,[rcx]; movaps [rip+view],xmm0;
// movups xmm1,[rcx+10h]; movaps [rip+view+10h],xmm1; movups xmm0,[rcx+20h]; movaps [rip+view+20h],xmm0. Rows: right,
// up, then the view's z axis, which points back (looking up lowers its y). Three other matrix copies in the exe have
// the same stores; this one comes right after mov rcx,[rdx]; lea rdx,[rdi+disp32] (the others after imul rax,rax,58h).
constexpr uint8_t kViewBefore[] = {0x48, 0x8B, 0x0A, 0x48, 0x8D, 0x97};
constexpr size_t kViewBeforeAt = 10;  // before the first store's movups
constexpr uint8_t kViewRow0[] = {0x0F, 0x10, 0x01, 0x0F, 0x29, 0x05};
constexpr uint8_t kViewRow1[] = {0x0F, 0x10, 0x49, 0x10, 0x0F, 0x29, 0x0D};
constexpr uint8_t kViewRow2[] = {0x0F, 0x10, 0x41, 0x20, 0x0F, 0x29, 0x05};
constexpr size_t kViewRow1At = 10, kViewRow2At = 21;
constexpr float kViewCut = 10;       // a view tilt this big in one update is a camera cut, not a turn (degrees)
constexpr float kDriveDecay = 0.8f;  // the pair's recent pitch input, for the camera's spring lag
constexpr float kScoreDecay = 0.98f;
constexpr float kScoreLearn = 2.0f;  // degrees^2 of agreement (or disagreement) that set a pair's relation

using Update = void (*)(void* odometer, uint64_t axes, uint64_t speeds, float dt, float* pair);
std::atomic<Update> g_original{nullptr};

// The gyro's turn so far (degrees, GyroLib's signs), added to by turn() on Gyro4All's frame thread.
std::atomic<double> g_total_yaw{0.0}, g_total_pitch{0.0};

struct Record {
    uintptr_t pair = 0;
    double applied_yaw = 0, applied_pitch = 0;  // of g_total_*, already added to this pair
    ULONGLONG last = 0;
    uint32_t calls = 0;
    int relation = 0;            // the view's tilt per pitch up on the pair: +1, -1, 0 = not learned yet (upright)
    float view = NAN;            // the view's pitch at the pair's last update (degrees, up positive)
    float drive = 0, score = 0;  // the pair's recent pitch input; its agreement with the view's tilt
};
constexpr int kRecords = 8;
std::mutex g_mutex;  // guards g_records (the update runs on the game's job threads)
Record g_records[kRecords];
uintptr_t g_view = 0;  // the static copy of the render view (rows right, up, back), 0 = not found: no learning
std::atomic<uint32_t> g_calls{0}, g_sync_timeouts{0}, g_inverted_calls{0};
std::atomic<ULONGLONG> g_last{0};
std::atomic<uintptr_t> g_pair{0};

// The view's pitch from the static copy (degrees, up positive), NaN when unreadable.
float view_pitch() {
    float back[3] = {};
    if (!g_view || !mem::rd(g_view + 0x20, back) || !(std::abs(back[1]) <= 1)) return NAN;
    return float(-std::asin(back[1]) * 180.0 / 3.14159265358979323846);
}

// Lock held: the pair's pitch input this update (the stick's and the gyro's) against the view's tilt since its last
// update.
void learn(Record& r, float pitch_input) {
    const float view = view_pitch(), last = r.view;
    r.view = view;
    if (std::isnan(view) || std::isnan(last)) return;
    const float tilt = view - last;
    if (std::abs(tilt) > kViewCut) {
        r.drive = r.score = 0;
        return;
    }
    r.drive = kDriveDecay * r.drive + pitch_input;
    r.score = kScoreDecay * r.score + r.drive * tilt;
    const int relation = r.score > kScoreLearn ? 1 : r.score < -kScoreLearn ? -1 : 0;
    if (!relation || relation == r.relation) return;
    r.relation = relation;
    log("fb: Andromeda: the pair at 0x%llX tilts the view %s when its pitch goes up: the gyro's pitch %s",
        (unsigned long long)r.pair, relation > 0 ? "up" : "down", relation > 0 ? "as is" : "inverted");
}

void hk_update(void* odometer, uint64_t axes, uint64_t speeds, float dt, float* pair) {
    float before[2] = {};
    const bool read_before = pair && mem::rd(reinterpret_cast<uintptr_t>(pair), before);
    g_original.load(std::memory_order_relaxed)(odometer, axes, speeds, dt, pair);
    ++g_calls;
    const auto at = reinterpret_cast<uintptr_t>(pair);
    if (!at) return;
    float after[2] = {};
    // The odometer's own pitch change (the stick's), wrapping at 360.
    const float pitch_change =
        read_before && mem::rd(at, after) ? std::remainder(after[1] - before[1], 360.0f) : 0.0f;
    if (!sync_gyro_frame(kSyncMs)) ++g_sync_timeouts;
    float add_yaw = 0, add_pitch = 0;
    int pitch_sign = 1;
    {
        std::lock_guard lock(g_mutex);
        const ULONGLONG now = GetTickCount64();
        Record* r = nullptr;
        for (Record& s : g_records)
            if (s.pair == at) r = &s;
        if (!r) {  // a new pair, or the oldest one's slot
            r = &g_records[0];
            for (Record& s : g_records)
                if (s.last < r->last) r = &s;
            *r = {};
            r->pair = at;
            r->applied_yaw = g_total_yaw.load(std::memory_order_relaxed);
            r->applied_pitch = g_total_pitch.load(std::memory_order_relaxed);
            log("fb: Andromeda: odometer 0x%llX turns the pair at 0x%llX (thread %lu)",
                (unsigned long long)reinterpret_cast<uintptr_t>(odometer), (unsigned long long)at,
                GetCurrentThreadId());
        }
        ++r->calls;
        r->last = now;
        if (r->relation < 0) pitch_sign = -1;
        const double yaw = g_total_yaw.load(std::memory_order_relaxed);
        const double pitch = g_total_pitch.load(std::memory_order_relaxed);
        add_yaw = float(yaw - r->applied_yaw);
        add_pitch = float(pitch - r->applied_pitch);
        r->applied_yaw = yaw;
        r->applied_pitch = pitch;
        learn(*r, pitch_change + pitch_sign * add_pitch);
        g_last = now;
        g_pair = at;
    }
    if (add_yaw == 0 && add_pitch == 0) return;
    if (pitch_sign < 0) ++g_inverted_calls;
    float angles[2] = {};
    if (!mem::rd(at, angles)) return;
    angles[0] -= add_yaw;
    angles[1] += pitch_sign * add_pitch;
    mem::guarded_store(at, angles, sizeof angles);
}

bool code_at(uintptr_t at, const uint8_t* bytes, size_t n) {
    uint8_t code[32] = {};
    return n <= sizeof code && mem::guarded_copy(code, at, n) && std::memcmp(code, bytes, n) == 0;
}

// Swaps a pointer in the exe's data (a vtable slot) for the hook, atomically.
bool swap_pointer(uintptr_t slot, void* hook, std::string& why) {
    auto* p = reinterpret_cast<void**>(slot);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) {
        why = std::format("can't make {} writable (error {})", mem::module_offset(slot), GetLastError());
        return false;
    }
    InterlockedExchangePointer(p, hook);
    VirtualProtect(p, sizeof(void*), old, &old);
    return true;
}

class MeaCamera final : public Camera {
public:
    const char* game() const override { return "Andromeda"; }

    // The odometer update, by its code, and the vtable slots that hold it.
    bool resolve(const TypeTable& types, std::string& why) override {
        if (!types.find("InputControllerAxesStrategy")) return why = "no InputControllerAxesStrategy type", false;
        std::vector<uintptr_t> starts;
        for (const uintptr_t at : find_pattern(kStart, sizeof kStart))
            if (code_at(at + kAccumulateAt, kAccumulate, sizeof kAccumulate) &&
                code_at(at + kAccumulatePitchAt, kAccumulatePitch, sizeof kAccumulatePitch))
                starts.push_back(at);
        if (starts.size() != 1)
            return why = std::format("the odometer update's code found {} times (needs 1)", starts.size()), false;
        slots_ = find_in_image({starts[0]});
        if (slots_.empty())
            return why = std::format("no vtable slot holds the odometer update at {}", mem::module_offset(starts[0])),
                   false;
        update_ = starts[0];
        find_view();
        return true;
    }

    // The static copy of the render view, for learning inverted cameras: the code that fills it (with its prefix)
    // occurs once and its three stores land 0x10 apart. Not found: every camera is taken as upright.
    static void find_view() {
        std::vector<uintptr_t> views;
        for (const uintptr_t at : find_pattern(kViewRow0, sizeof kViewRow0)) {
            if (!code_at(at - kViewBeforeAt, kViewBefore, sizeof kViewBefore) ||
                !code_at(at + kViewRow1At, kViewRow1, sizeof kViewRow1) ||
                !code_at(at + kViewRow2At, kViewRow2, sizeof kViewRow2))
                continue;
            const auto target = [&](size_t store) {  // the rip-relative store's address (7-byte movaps)
                int32_t rel = 0;
                return mem::rd(at + store + 3, rel) ? at + store + 7 + rel : uintptr_t(0);
            };
            const uintptr_t view = target(3);
            if (view && target(kViewRow1At + 4) == view + 0x10 && target(kViewRow2At + 4) == view + 0x20)
                views.push_back(view);
        }
        if (views.size() == 1) {
            g_view = views[0];
            log("fb: Andromeda: render view copy at %s: inverted cameras are learned from it",
                mem::module_offset(g_view).c_str());
        } else {
            log("fb: Andromeda: the render view copy's code found %zu times (needs 1): inverted cameras aren't detected",
                views.size());
        }
    }

    bool install(std::string& why) override {
        g_original = reinterpret_cast<Update>(update_);  // before the swap: a call may come right after it
        std::string slots;
        for (const uintptr_t slot : slots_) {
            if (!swap_pointer(slot, reinterpret_cast<void*>(&hk_update), why)) return false;
            slots += " " + mem::module_offset(slot);
        }
        log("fb: Andromeda: hooked the camera's odometer update %s (vtable slot%s at%s)",
            mem::module_offset(update_).c_str(), slots_.size() > 1 ? "s" : "", slots.c_str());
        hooked_ = true;
        return true;
    }

    // Degrees, yaw right / pitch up positive; hk_update gives each pair its share with the pair's signs.
    bool turn(double pitch, double yaw) override {
        if (!hooked_ || GetTickCount64() - g_last.load(std::memory_order_relaxed) > kRecentMs) return false;
        g_total_yaw.fetch_add(yaw);
        g_total_pitch.fetch_add(pitch);
        return true;
    }

    CameraFrame read() override {
        CameraFrame f;
        const bool running = GetTickCount64() - g_last.load(std::memory_order_relaxed) <= kRecentMs;
        f.in_game = running;
        f.active = running;
        f.target = running ? std::format("camera input 0x{:X}", g_pair.load())
                           : std::string(hooked_ ? "no camera input running" : "not hooked");
        return f;
    }

    uintptr_t watch_address() const override { return g_pair.load(std::memory_order_relaxed); }

    std::string status() const override {
        int pairs = 0, inverted = 0;
        {
            std::lock_guard lock(g_mutex);
            const ULONGLONG now = GetTickCount64();
            for (const Record& r : g_records)
                if (r.pair && now - r.last <= 1000) {
                    ++pairs;
                    inverted += r.relation < 0 ? 1 : 0;
                }
        }
        return std::format("camera: Andromeda, {}; odometer updates {} ({} without the gyro's frame in time, {} with "
                           "the pitch inverted), pairs now {} ({} inverted), gyro turn so far yaw {:.1f} pitch {:.1f} deg",
                           hooked_ ? "hooked" : "not hooked", g_calls.load(), g_sync_timeouts.load(),
                           g_inverted_calls.load(), pairs, inverted, g_total_yaw.load(), g_total_pitch.load());
    }

private:
    uintptr_t update_ = 0;
    std::vector<uintptr_t> slots_;
    bool hooked_ = false;
};

}  // namespace

std::unique_ptr<Camera> make_mea_camera() { return std::make_unique<MeaCamera>(); }

}  // namespace gyro4all::fb
