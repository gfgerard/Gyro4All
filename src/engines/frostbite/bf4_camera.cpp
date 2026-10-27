// Battlefield 4 and Battlefield Hardline (MODLOG 2026-10-21; the same update in both, Hardline's at bfh.exe+0x4388620
// in build 612926, other registers). The soldier's aim is a small record (no vtable) whose yaw and pitch, radians, sit
// at +0x14 / +0x18; every tick the game's aim update exe+0xFBA340 (EA app build 179547) reads them and the look input,
// computes the new angles (yaw wrapped, pitch clamped), stores a target at +0x60 / +0x64 and returns the turn as a
// delta. Written from outside, those angles turn the camera and the gun together, but most writes are lost to the
// race with the update (read, compute, write back). So the gyro goes in from inside it: the update is hooked, and the
// gyro's turn since the last call goes in around the game's code (where: "Two rates" below).
// - Two rates: the aim update runs every rendered frame, the game's simulation ticks at 30 Hz (exe+0xFB9610). Right
//   after a tick (time since it, +0x68, zero) the update starts from the stored angles; on the frames after, with
//   extrapolation on (the input record's +0x188), from the last target (+0x60 / +0x64), and the next tick copies that
//   target into the stored angles. The update returns this frame's view offset as a delta (new angles - stored angles):
//   that's what makes the view move smoothly between ticks. So, with extrapolation on, the turn is added after the
//   game's code, to the returned delta (this frame's view) and to the target (the next frames, and the tick's commit);
//   the stored angles are left to the tick. (Adding to the stored angles only kept about a quarter of the turn, in
//   30 Hz steps; adding to them and to the target cancelled in the delta, so the view moved only on the 30 Hz ticks.)
//   With extrapolation off the update always starts from the stored angles: the turn goes there, before.
// - Found at run time by its code (no exe addresses): the update's first 29 bytes (prologue, `mov rax,[r8+0Ch]`, the
//   `movss xmm6,[rip+..]` of its 1e-6 epsilon; either encoding of `mov r11,rsp`) occur once in the exe, and its
//   target stores (+0x60/+0x64/+0x68 on one register) are checked. The
//   exe's code is unpacked by the time the attach thread runs. An inline hook (MinHook): the call is direct, there is no
//   pointer to swap.
// - Whose aim: AI soldiers may run the same update on their own records. The player's records are the ones whose look
//   input moves while the controller Gyro4All shows as XInput has its right stick pushed, so the gyro waits until the
//   right stick has moved once. Every player record (the update runs about twice per frame) gets the same total turn:
//   each keeps how much of it it already has.
// - Signs: the gyro's turn goes in with its own signs (yaw right, pitch up positive): in game the opposite turned the
//   view the wrong way on both axes (user-seen), though the camera transform suggested stored yaw grows to the left
//   (its yaw = 180 deg - stored yaw) and stored pitch is negative looking up.
// - Timing: Gyro4All's frame (GyroLib) runs on its own thread after each Present, the aim update on a job thread some
//   time into the next frame, so a frame's turn would land in one update or the next (some frames none, some two: a
//   judder at 150 fps). So the player's aim update first wakes the frame and waits for it (sync_gyro_frame, ~0.1 ms,
//   at most kSyncMs), and applies the turn up to that moment.
// - Gate: the player's aim updated in the last 100 ms (none in menus, pause, cutscenes).
#include "fb_camera.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "xinput.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <utility>

namespace gyro4all::fb {

namespace {

// The aim update's start after `mov r11,rsp` (4C 8B DC in Battlefield 4, 49 89 E3 in Hardline): mov [r11+18h],rbx;
// mov [r11+20h],rsi; push rdi; sub rsp,70h; mov rax,[r8+0Ch]; movaps [rsp+60h],xmm6; movss xmm6,[rip+rel32].
constexpr uint8_t kStart[] = {0x49, 0x89, 0x5B, 0x18, 0x49, 0x89, 0x73, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70,
                              0x49, 0x8B, 0x40, 0x0C, 0x0F, 0x29, 0x74, 0x24, 0x60, 0xF3, 0x0F, 0x10, 0x35};
constexpr uint8_t kMovR11Rsp[][3] = {{0x4C, 0x8B, 0xDC}, {0x49, 0x89, 0xE3}};
constexpr size_t kBodySize = 0x300;  // where the target stores are looked for (BF4 +0x1E9, Hardline +0x265)
constexpr uintptr_t kYaw = 0x14;                   // the aim record's angles (yaw, then pitch; radians)
constexpr uintptr_t kTargetYaw = 0x60;             // the last target (yaw, then pitch), read between ticks
constexpr uintptr_t kLookX = 0x0C, kLookY = 0x10;  // the input record's look input
constexpr uintptr_t kExtrapolate = 0x188;          // the input record's flag: extrapolate between ticks (byte)
constexpr float kLookMoving = 0.2f;                // look input that counts as moving
constexpr ULONGLONG kRecentMs = 100;
constexpr unsigned kSyncMs = 4;  // longest wait for Gyro4All's frame in the aim update
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

using Update = void* (*)(uintptr_t aim, float* delta, uintptr_t input);
Update g_original = nullptr;

// The gyro's turn so far (radians, the record's signs), added to by turn() on Gyro4All's frame thread.
std::atomic<double> g_total_yaw{0.0}, g_total_pitch{0.0};

struct Record {
    uintptr_t aim = 0;
    bool player = false;
    double applied_yaw = 0, applied_pitch = 0;  // of g_total_*, already added to this record
    ULONGLONG last = 0;
    uint32_t calls = 0;
    DWORD thread = 0;
};
constexpr int kRecords = 16;
std::mutex g_mutex;  // guards g_records (the update runs on the game's job threads)
Record g_records[kRecords];
std::atomic<uint32_t> g_calls{0}, g_player_calls{0}, g_sync_timeouts{0};
std::atomic<ULONGLONG> g_player_last{0}, g_any_last{0};
std::atomic<uintptr_t> g_player_aim{0};

void* hk_update(uintptr_t aim, float* delta, uintptr_t input) {
    ++g_calls;
    const ULONGLONG now = GetTickCount64();
    g_any_last = now;
    float look_x = 0, look_y = 0;
    mem::rd(input + kLookX, look_x);
    mem::rd(input + kLookY, look_y);
    const bool looking = std::abs(look_x) > kLookMoving || std::abs(look_y) > kLookMoving;
    const bool pushed = looking && xinput::right_stick_pushed();
    float add_yaw = 0, add_pitch = 0;  // the gyro's turn since this record's last call (the player's only)
    // The player's record: bring the gyro's turn up to now first (outside the lock: the frame calls turn()).
    if (aim == g_player_aim.load(std::memory_order_relaxed) && !sync_gyro_frame(kSyncMs)) ++g_sync_timeouts;
    {
        std::lock_guard lock(g_mutex);
        Record* r = nullptr;
        for (Record& s : g_records)
            if (s.aim == aim) r = &s;
        if (!r) {  // a new record, or the oldest one's slot (records come and go with soldiers)
            r = &g_records[0];
            for (Record& s : g_records)
                if (s.last < r->last) r = &s;
            *r = {};
            r->aim = aim;
            r->applied_yaw = g_total_yaw.load(std::memory_order_relaxed);
            r->applied_pitch = g_total_pitch.load(std::memory_order_relaxed);
            r->thread = GetCurrentThreadId();
        }
        ++r->calls;
        r->last = now;
        if (pushed && !r->player) {
            r->player = true;
            log("fb: Battlefield: aim record 0x%llX is the player's (its look input moves with the right stick; "
                "thread %lu)", (unsigned long long)aim, r->thread);
        }
        if (r->player) {
            ++g_player_calls;
            g_player_last = now;
            g_player_aim = aim;
            const double yaw = g_total_yaw.load(std::memory_order_relaxed);
            const double pitch = g_total_pitch.load(std::memory_order_relaxed);
            add_yaw = float(yaw - r->applied_yaw);
            add_pitch = float(pitch - r->applied_pitch);
            r->applied_yaw = yaw;
            r->applied_pitch = pitch;
        }
    }
    const auto add = [&](uintptr_t pair) {
        float angles[2] = {};
        if (!mem::rd(pair, angles)) return;
        angles[0] += add_yaw;
        angles[1] += add_pitch;
        mem::guarded_store(pair, angles, sizeof angles);
    };
    const bool turning = add_yaw != 0 || add_pitch != 0;
    uint8_t extrapolating = 1;
    mem::rd(input + kExtrapolate, extrapolating);
    if (turning && !extrapolating) add(aim + kYaw);  // the update always starts from the stored angles
    void* const result = g_original(aim, delta, input);
    if (turning && extrapolating) {
        // This frame's view (the delta), and the target the next frames start from and the tick commits.
        add(reinterpret_cast<uintptr_t>(delta));
        add(aim + kTargetYaw);
    }
    return result;
}

// The target stores in the update's code: `movss [r+60h],xmm` and `movss [r+64h],xmm` (either first: BF4 stores yaw
// first, Hardline pitch) and then `mov [r+68h],eax` (the time), on the same base register, each within 0x20 bytes of
// the one before. The registers differ between games.
bool stores_target(const uint8_t* code, size_t n) {
    // movss [base+disp8],xmm (F3 0F 11, mod 01): its base register, or -1.
    const auto movss_store = [&](size_t i, uint8_t disp) {
        return i + 5 <= n && code[i] == 0xF3 && code[i + 1] == 0x0F && code[i + 2] == 0x11 &&
                       (code[i + 3] & 0xC0) == 0x40 && (code[i + 3] & 7) != 4 && code[i + 4] == disp
                   ? code[i + 3] & 7
                   : -1;
    };
    for (size_t i = 0; i < n; ++i) {
        for (const auto [first, second] : {std::pair<uint8_t, uint8_t>{0x60, 0x64}, {0x64, 0x60}}) {
            const int base = movss_store(i, first);
            if (base < 0) continue;
            for (size_t j = i + 5; j < std::min(n, i + 0x20); ++j) {
                if (movss_store(j, second) != base) continue;
                for (size_t k = j + 5; k + 3 <= std::min(n, j + 0x20); ++k)
                    if (code[k] == 0x89 && code[k + 1] == (0x40 | base) && code[k + 2] == 0x68) return true;
            }
        }
    }
    return false;
}

class Bf4Camera final : public Camera {
public:
    const char* game() const override { return "Battlefield"; }

    // The aim update, by its code: the start occurs once and the target stores are where they should be.
    bool resolve(const TypeTable& types, std::string& why) override {
        if (!types.find("ClientSoldierEntity")) return why = "no ClientSoldierEntity type", false;
        std::vector<uintptr_t> starts;
        for (const uintptr_t after : find_pattern(kStart, sizeof kStart)) {
            uint8_t head[3] = {};
            if (!mem::guarded_copy(head, after - 3, 3)) continue;
            for (const auto& mov : kMovR11Rsp)
                if (std::memcmp(head, mov, 3) == 0) starts.push_back(after - 3);
        }
        if (starts.size() != 1) return why = std::format("the aim update's code found {} times (needs 1)", starts.size()), false;
        uint8_t body[kBodySize] = {};
        if (!mem::guarded_copy(body, starts[0], sizeof body) || !stores_target(body, sizeof body))
            return why = std::format("the aim update at {} doesn't store its target at +0x60/+0x64/+0x68",
                                     mem::module_offset(starts[0])),
                   false;
        update_ = starts[0];
        return true;
    }

    bool install(std::string& why) override {
        if (!hooks::hook_function(reinterpret_cast<void*>(update_), reinterpret_cast<void*>(&hk_update),
                                  reinterpret_cast<void**>(&g_original)))
            return why = "the aim update could not be hooked", false;
        log("fb: Battlefield: hooked the soldier's aim update at %s; the gyro starts once the right stick has moved",
            mem::module_offset(update_).c_str());
        hooked_ = true;
        return true;
    }

    // Degrees, yaw right / pitch up positive -> the record's radians, same signs (see the top).
    bool turn(double pitch, double yaw) override {
        if (!hooked_ || GetTickCount64() - g_player_last.load(std::memory_order_relaxed) > kRecentMs) return false;
        g_total_yaw.fetch_add(yaw * kDegToRad);
        g_total_pitch.fetch_add(pitch * kDegToRad);
        return true;
    }

    CameraFrame read() override {
        CameraFrame f;
        const ULONGLONG now = GetTickCount64();
        const bool any = now - g_any_last.load(std::memory_order_relaxed) <= kRecentMs;
        const bool player = now - g_player_last.load(std::memory_order_relaxed) <= kRecentMs;
        f.in_game = any;
        f.active = player;
        f.target = player ? std::format("aim 0x{:X}", g_player_aim.load())
                   : any  ? std::string("aim updates (move the right stick once)")
                          : std::string(hooked_ ? "no aim update running" : "not hooked");
        return f;
    }

    uintptr_t watch_address() const override {
        const uintptr_t aim = g_player_aim.load(std::memory_order_relaxed);
        return aim ? aim + kYaw : 0;
    }

    std::string status() const override {
        int records = 0, players = 0;
        {
            std::lock_guard lock(g_mutex);
            const ULONGLONG now = GetTickCount64();
            for (const Record& r : g_records)
                if (r.aim && now - r.last <= 1000) ++records, players += r.player ? 1 : 0;
        }
        return std::format("camera: Battlefield, {}; aim updates {} ({} for the player, {} without the gyro's frame "
                           "in time), records now {} ({} the player's), gyro turn so far yaw {:.1f} pitch {:.1f} deg",
                           hooked_ ? "hooked" : "not hooked", g_calls.load(), g_player_calls.load(),
                           g_sync_timeouts.load(), records, players,
                           g_total_yaw.load() / kDegToRad, g_total_pitch.load() / kDegToRad);
    }

private:
    uintptr_t update_ = 0;
    bool hooked_ = false;
};

}  // namespace

std::unique_ptr<Camera> make_bf4_camera() { return std::make_unique<Bf4Camera>(); }

}  // namespace gyro4all::fb
