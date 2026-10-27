// Dead Space (2023) (MODLOG 2026-10-18). Isaac and his camera turn from the character's input actions EIAYaw / EIAPitch:
// the player's input update fills the input state with the action levels every tick, and forcing those levels turns
// Isaac, his arm and the camera together exactly like the stick, on foot and aiming. (Turning the Chimera camera's
// rotation interface directly only moved the camera: Isaac followed in jerks and, aiming, only the weapon camera moved
// inside the torso's limits.) So the gyro is written as action levels, at the one moment they count:
// - The levels are rebuilt from the controller every tick by the mapper and used within the same input update, so a
//   write from outside the update never lands (tried: after the update, before its copy out, into the copy). Within it,
//   for each player-input object P, exe+0x1A73BE0 (Steam build) calls the mapper and right after it P's vtable slot 16
//   with (P, dt, the input state); every consumer found offline comes after that call (traced in the Steam build, whose
//   code is plain on disk: tools/frostbite/exefile.py). Slot 16 of P's vtable is swapped for a hook that adds the gyro's
//   turn to this tick's levels and then calls the game's: data in the exe, no code changed.
// - P is found at run time: the player's input update runs through a 16-byte delegate invoker (`mov rdx,[rcx+38h];
//   mov rcx,[rcx+30h]; jmp <update>`) whose address sits in the exe's writable data; that qword is swapped for an invoker
//   of our own that reads P = [[this+0x90]+0x118] (this = [context+0x30]) and its state [P+0x148], hooks P's vtable on
//   the first call (its slot 16 code checked first), and calls the game's.
// - The levels are floats at state+0x500 + action index * 4 (EIAYaw 11, EIAPitch 8 in the game's action map). A level
//   turns at a speed given by the game's stick response curve (measured: convex, capped at level 1), so the gyro's
//   degrees this tick become a speed, are added to the speed of the level the player's stick already gave, and go back
//   through the inverse curve.
// Gyro4All's frame (GyroLib, its own thread) only queues the turn (turn()); the input update runs on the game's job
// threads. HybridRotationInputChimeraBehavior's update (the Chimera camera's stick behaviour) is hooked too, through its
// vtable, only to know when a gameplay camera runs (none in menus, pause and cutscenes).
#include "fb_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace gyro4all::fb {

namespace {

// ---- the camera behaviour (gameplay camera running)

constexpr const char* kBehaviour = "HybridRotationInputChimeraBehavior";
// getType() is slot 0 of the class's vtable; the per-frame update is slot 16.
constexpr int kUpdateSlot = 16;
// The update starts `push rbx; sub rsp,20h; cmp byte ptr [rcx+119h],0` (the behaviour's cached "active" flag): checked
// at +6 so a different build is noticed before anything relies on the layout.
constexpr uint8_t kUpdateCheck[] = {0x80, 0xB9, 0x19, 0x01, 0x00, 0x00, 0x00};
constexpr size_t kUpdateCheckAt = 6;
// Behaviour layout (read by the update): +0x08 its data (+0x30 RotationReferenceType: 0 axes, 1 transform), +0xF8 the
// rotation interface it turns, +0x118 the cached "active" flag and +0x119 whether that cache is valid.
constexpr uintptr_t kData = 0x08, kDataReference = 0x30, kInterface = 0xF8, kActive = 0x118, kActiveValid = 0x119;

// ---- the player's input update, the player-input object and the input state

// The delegate invoker family: `mov rdx,[rcx+38h]; mov rcx,[rcx+30h]; jmp rel32` (41 of them, one per target).
constexpr uint8_t kInvoker[] = {0x48, 0x8B, 0x51, 0x38, 0x48, 0x8B, 0x49, 0x30, 0xE9};
constexpr size_t kInvokerSize = sizeof kInvoker + 4;
// The player's input update, at +0xF: `mov r8d,[rdx+20h]; xorps xmm0,xmm0; cvtsi2sd xmm0,qword ptr [rdx]; mov rbx,rcx;
// mov rcx,[rcx+208h]; mov rsi,rdx`.
constexpr uint8_t kInputUpdateCheck[] = {0x44, 0x8B, 0x42, 0x20, 0x0F, 0x57, 0xC0, 0xF2, 0x48, 0x0F, 0x2A, 0x02, 0x48,
                                         0x8B, 0xD9, 0x48, 0x8B, 0x89, 0x08, 0x02, 0x00, 0x00, 0x48, 0x8B, 0xF2};
constexpr size_t kInputUpdateCheckAt = 0xF;
constexpr uintptr_t kContextThis = 0x30;
constexpr uintptr_t kPlayerPath[] = {0x90, 0x118};  // the update's this -> the input manager -> the player-input object
constexpr uintptr_t kPlayerState = 0x148;           // player-input object -> its input state
// The player-input object's slot 16 (exe+0x1A6F150 in the Steam build), called right after the mapper with (P, dt,
// state): it walks the input components and calls their vf48 with the state. Its code is checked in three pieces (the
// rest are rip-relative loads): the prologue up to `mov eax,[rip+..]`, `mov rsi,rcx; mov rdi,[rip+..]`, `sub rsi,rax;
// xor ebx,ebx`.
constexpr int kPlayerSlot = 16;
constexpr uint8_t kPlayerCheckA[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                     0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x05};
constexpr uint8_t kPlayerCheckB[] = {0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x3D};
constexpr uint8_t kPlayerCheckC[] = {0x48, 0x2B, 0xF0, 0x33, 0xDB};
constexpr size_t kPlayerCheckBAt = 0x1A, kPlayerCheckCAt = 0x24;
constexpr uintptr_t kLevels = 0x500;
constexpr int kYawAction = 11, kPitchAction = 8;  // EIAYaw, EIAPitch in the game's action map

// Turn speed (degrees per second) per action level, measured in game with the level forced (tools/frostbite/
// levelrate.py): the same yaw curve on foot and aiming, the pitch at about 0.7x; capped at level 1. Positive = right /
// up, as for the stick.
struct Point {
    double level, speed;
};
constexpr Point kYawCurve[] = {{0, 0}, {0.25, 11}, {0.5, 24}, {0.6, 31}, {0.7, 43}, {0.8, 63}, {0.9, 105}, {1, 180}};
constexpr Point kPitchCurve[] = {{0, 0}, {0.25, 8}, {0.5, 17}, {0.7, 29}, {0.9, 70}, {1, 110}};

template <size_t N>
double speed_of(const Point (&curve)[N], double level) {
    const double a = std::min(std::abs(level), 1.0);
    for (size_t i = 1; i < N; ++i)
        if (a <= curve[i].level) {
            const Point &p = curve[i - 1], &q = curve[i];
            return std::copysign(p.speed + (a - p.level) / (q.level - p.level) * (q.speed - p.speed), level);
        }
    return std::copysign(curve[N - 1].speed, level);
}

template <size_t N>
double level_of(const Point (&curve)[N], double speed) {
    const double s = std::abs(speed);
    for (size_t i = 1; i < N; ++i)
        if (s <= curve[i].speed) {
            const Point &p = curve[i - 1], &q = curve[i];
            return std::copysign(p.level + (s - p.speed) / (q.speed - p.speed) * (q.level - p.level), speed);
        }
    return std::copysign(1.0, speed);  // faster than the game turns at full stick: capped
}

constexpr ULONGLONG kRecentMs = 100;  // an update this recent counts as running; a turn this old is dropped
constexpr ULONGLONG kGoneMs = 1000;   // a slot this stale is reused

// One behaviour object seen running. The hook (game job threads) claims a slot for a new object and refreshes it on
// every call; Gyro4All's frame thread reads them. Racy by design (a coarse "is it running" test): a torn read costs one
// frame.
struct Seen {
    std::atomic<uintptr_t> self{0};
    std::atomic<ULONGLONG> last{0};
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> thread{0};
    std::atomic<uintptr_t> iface{0};
    std::atomic<int32_t> active{-1};    // the cached flag, -1 = the cache isn't valid
    std::atomic<int32_t> reference{-1}; // RotationReferenceType, -1 = unreadable
    // Frame thread only: what was last logged for this slot.
    uintptr_t logged_self = 0, logged_iface = 0;
    int32_t logged_active = -2, logged_reference = -2;
};
constexpr int kSeen = 16;
Seen g_seen[kSeen];
std::atomic<uint64_t> g_calls{0}, g_dropped{0};
std::atomic<uintptr_t> g_newest{0};  // the newest running behaviour object (a mode change makes a new one)

using Update = void (*)(void* self, uintptr_t a, uintptr_t b, uintptr_t c);
std::atomic<Update> g_original{nullptr};

// The gyro's turn waiting for the next input tick (degrees, GyroLib's sense: yaw right, pitch up), and when queued.
std::atomic<double> g_pending_yaw{0}, g_pending_pitch{0};
std::atomic<ULONGLONG> g_pending_at{0};

using Invoke = void (*)(void* context);
std::atomic<Invoke> g_input_original{nullptr};
// The player-input object's slot 16: (P, dt in xmm1, state in r8, r9 passed through); returns the components' result.
using PlayerSlot = int (*)(void* self, float dt, void* state, uintptr_t r9);
std::atomic<PlayerSlot> g_player_original{nullptr};
std::atomic<uintptr_t> g_player{0}, g_state{0}, g_player_vtable{0};
std::atomic<int> g_player_hook{0};  // 0 not tried, 1 being installed, 2 hooked, 3 failed
std::atomic<int64_t> g_last_tick{0};  // QueryPerformanceCounter at the last injection tick
std::atomic<uint64_t> g_input_calls{0}, g_player_calls{0}, g_state_calls{0}, g_level_writes{0}, g_capped{0};
int64_t g_qpc_hz = 1;
uintptr_t g_image_begin = 0, g_image_end = 0;

bool in_image(uintptr_t p) { return p >= g_image_begin && p < g_image_end; }

Seen* note(uintptr_t self) {
    const ULONGLONG now = GetTickCount64();
    Seen* slot = nullptr;
    for (Seen& s : g_seen)
        if (s.self.load(std::memory_order_relaxed) == self) {
            slot = &s;
            break;
        }
    if (!slot) {
        for (Seen& s : g_seen) {
            uintptr_t old = s.self.load(std::memory_order_relaxed);
            if ((old == 0 || now - s.last.load(std::memory_order_relaxed) > kGoneMs) &&
                s.self.compare_exchange_strong(old, self)) {
                s.calls = 0;
                g_newest = self;  // a new object: the camera mode changed to it
                slot = &s;
                break;
            }
        }
        if (!slot) {
            g_dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }
    uint8_t valid = 0, active = 0;
    uintptr_t data = 0, iface = 0;
    uint32_t reference = 0;
    const bool cache = mem::rd(self + kActiveValid, valid) && mem::rd(self + kActive, active);
    slot->active.store(cache && valid ? active : -1, std::memory_order_relaxed);
    slot->reference.store(
        (data = mem::rdp(self + kData)) && mem::rd(data + kDataReference, reference) ? int32_t(reference) : -1,
        std::memory_order_relaxed);
    slot->iface.store(mem::rd(self + kInterface, iface) ? iface : 0, std::memory_order_relaxed);
    slot->thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    slot->calls.fetch_add(1, std::memory_order_relaxed);
    slot->last.store(now, std::memory_order_relaxed);
    // The newest object stopped (its mode ended without a new one): whichever object still runs takes over.
    const uintptr_t newest = g_newest.load(std::memory_order_relaxed);
    if (newest != self) {
        bool newest_runs = false;
        for (Seen& s : g_seen)
            if (s.self.load(std::memory_order_relaxed) == newest)
                newest_runs = now - s.last.load(std::memory_order_relaxed) <= kRecentMs;
        if (!newest_runs) g_newest = self;
    }
    return slot;
}

void hk_update(void* self, uintptr_t a, uintptr_t b, uintptr_t c) {
    g_original.load(std::memory_order_relaxed)(self, a, b, c);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    note(reinterpret_cast<uintptr_t>(self));
}

// Right after the mapper wrote this tick's levels, before the game uses them: the gyro's pending turn added to the
// player's EIAYaw / EIAPitch levels. game_dt is the tick's dt as the game passes it (seconds, checked against the time
// between ticks in the first log line).
void inject(uintptr_t state, float game_dt) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t last = g_last_tick.exchange(now.QuadPart, std::memory_order_relaxed);
    const double measured = last ? double(now.QuadPart - last) / double(g_qpc_hz) : 0;
    const double dt =
        game_dt >= 0.001f && game_dt <= 0.2f ? game_dt : std::clamp(measured ? measured : 1.0 / 30, 0.004, 0.1);
    if (GetTickCount64() - g_pending_at.load(std::memory_order_relaxed) > kRecentMs) return;  // nothing queued lately
    const double yaw = g_pending_yaw.exchange(0.0), pitch = g_pending_pitch.exchange(0.0);
    if (yaw == 0 && pitch == 0) return;
    const uintptr_t yaw_at = state + kLevels + 4 * kYawAction, pitch_at = state + kLevels + 4 * kPitchAction;
    float yaw_level = 0, pitch_level = 0;  // the player's stick, just mapped
    if (!mem::rd(yaw_at, yaw_level) || !mem::rd(pitch_at, pitch_level)) return;
    const double want_yaw = speed_of(kYawCurve, yaw_level) + yaw / dt;
    const double want_pitch = speed_of(kPitchCurve, pitch_level) + pitch / dt;
    const float new_yaw = float(level_of(kYawCurve, want_yaw));
    const float new_pitch = float(level_of(kPitchCurve, want_pitch));
    if (std::abs(want_yaw) > kYawCurve[std::size(kYawCurve) - 1].speed ||
        std::abs(want_pitch) > kPitchCurve[std::size(kPitchCurve) - 1].speed)
        g_capped.fetch_add(1, std::memory_order_relaxed);
    if (mem::guarded_store(yaw_at, &new_yaw, 4) && mem::guarded_store(pitch_at, &new_pitch, 4)) {
        if (g_level_writes.fetch_add(1, std::memory_order_relaxed) == 0)
            log("fb: Dead Space: first gyro levels: turn (%.3f, %.3f) deg, game dt %.1f ms (%.1f ms since the last tick) "
                "-> EIAYaw %.3f -> %.3f, EIAPitch %.3f -> %.3f",
                yaw, pitch, game_dt * 1000.0, measured * 1000, yaw_level, new_yaw, pitch_level, new_pitch);
    }
}

int hk_player(void* self, float dt, void* state, uintptr_t r9) {
    g_player_calls.fetch_add(1, std::memory_order_relaxed);
    const auto s = reinterpret_cast<uintptr_t>(state);
    if (s && s == g_state.load(std::memory_order_relaxed)) {  // the player's own state, not another input object's
        g_state_calls.fetch_add(1, std::memory_order_relaxed);
        inject(s, dt);
    }
    return g_player_original.load(std::memory_order_relaxed)(self, dt, state, r9);
}

bool executable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool writable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

std::string hex_bytes(const uint8_t* data, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += std::format("{}{:02X}", i ? " " : "", data[i]);
    return out;
}

bool code_at(uintptr_t at, const uint8_t* bytes, size_t n) {
    uint8_t code[64] = {};
    return n <= sizeof code && mem::guarded_copy(code, at, n) && std::memcmp(code, bytes, n) == 0;
}

const char* reference_name(int32_t r) { return r == 0 ? "axes" : r == 1 ? "transform" : "?"; }

// Swaps a pointer in the exe's data (a vtable slot or a delegate's invoker) for a hook, atomically.
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

// On the first input update that has a player-input object: its vtable's slot 16 checked and swapped for hk_player.
// One thread installs; the others skip until it is done.
void hook_player(uintptr_t player) {
    int expected = 0;
    if (!g_player_hook.compare_exchange_strong(expected, 1)) {
        // Already hooked: note a player-input object of another class (its slot 16 isn't hooked), once.
        static std::atomic<bool> told{false};
        const uintptr_t vtable = mem::rdp(player);
        if (expected == 2 && vtable && vtable != g_player_vtable.load(std::memory_order_relaxed) && !told.exchange(true))
            log("fb: Dead Space: player-input object 0x%llX has another vtable (%s), not hooked",
                (unsigned long long)player, mem::module_offset(vtable).c_str());
        return;
    }
    const uintptr_t vtable = mem::rdp(player);
    uintptr_t target = 0;
    uint8_t code[48] = {};
    const bool read = in_image(vtable) && mem::rd(vtable + 8 * kPlayerSlot, target) && in_image(target) &&
                      executable(target) && mem::guarded_copy(code, target, sizeof code);
    const bool match = read && code_at(target, kPlayerCheckA, sizeof kPlayerCheckA) &&
                       code_at(target + kPlayerCheckBAt, kPlayerCheckB, sizeof kPlayerCheckB) &&
                       code_at(target + kPlayerCheckCAt, kPlayerCheckC, sizeof kPlayerCheckC);
    log("fb: Dead Space: player-input object 0x%llX, vtable %s, slot %d %s: %s (%s)", (unsigned long long)player,
        mem::module_offset(vtable).c_str(), kPlayerSlot, mem::module_offset(target).c_str(),
        read ? hex_bytes(code, sizeof code).c_str() : "unreadable", match ? "the expected code" : "NOT the expected code");
    std::string why;
    if (match) {
        g_player_original = reinterpret_cast<PlayerSlot>(target);  // before the swap: a call may come right after it
        if (swap_pointer(vtable + 8 * kPlayerSlot, reinterpret_cast<void*>(&hk_player), why)) {
            g_player_vtable = vtable;
            g_player_hook = 2;
            log("fb: Dead Space: player-input slot %d hooked (at %s)", kPlayerSlot,
                mem::module_offset(vtable + 8 * kPlayerSlot).c_str());
            return;
        }
        log("fb: Dead Space: player-input slot not hooked: %s", why.c_str());
    }
    g_player_hook = 3;
}

// Before the game's input update: the player-input object and its state, and the slot-16 hook on the first call.
void hk_input(void* context) {
    g_input_calls.fetch_add(1, std::memory_order_relaxed);
    uintptr_t player = mem::rdp(reinterpret_cast<uintptr_t>(context) + kContextThis);
    for (uintptr_t offset : kPlayerPath) player = player ? mem::rdp(player + offset) : 0;
    const uintptr_t state = player ? mem::rdp(player + kPlayerState) : 0;
    if (state) {
        if (g_player.exchange(player, std::memory_order_relaxed) != player ||
            g_state.exchange(state, std::memory_order_relaxed) != state)
            log("fb: Dead Space: player-input object 0x%llX, input state 0x%llX", (unsigned long long)player,
                (unsigned long long)state);
        if (g_player_hook.load(std::memory_order_relaxed) != 3) hook_player(player);
    }
    g_input_original.load(std::memory_order_relaxed)(context);
}

class DeadSpaceCamera final : public Camera {
public:
    const char* game() const override { return "Dead Space"; }

    bool resolve(const TypeTable& types, std::string& why) override {
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        g_qpc_hz = hz.QuadPart;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_image_begin = base;
        g_image_end = base + nt->OptionalHeader.SizeOfImage;
        return resolve_behaviour(types, why) && resolve_input(why);
    }

    // Both hooks swap a pointer in the exe's data: no code is changed. The originals are set first, since a call may
    // come right after a swap. The player-input object's slot follows from the first input update (hook_player).
    bool install(std::string& why) override {
        g_original = reinterpret_cast<Update>(update_);
        if (!swap_pointer(vtable_ + 8 * kUpdateSlot, reinterpret_cast<void*>(&hk_update), why)) return false;
        g_input_original = reinterpret_cast<Invoke>(input_.invoker);
        if (!swap_pointer(input_.slot, reinterpret_cast<void*>(&hk_input), why)) return false;
        log("fb: Dead Space: hooked the camera behaviour's update (vtable slot %d) and the player's input update "
            "(invoker at %s)",
            kUpdateSlot, mem::module_offset(input_.slot).c_str());
        hooked_ = true;
        return true;
    }

    // Queued for the next input tick, while a gameplay camera runs and the player-input slot is hooked.
    bool turn(double pitch, double yaw) override {
        if (!hooked_ || g_player_hook.load(std::memory_order_relaxed) != 2 || !g_newest.load(std::memory_order_relaxed))
            return false;
        const ULONGLONG now = GetTickCount64();
        if (now - g_pending_at.load(std::memory_order_relaxed) > kRecentMs) {  // stale: dropped, not added to
            g_pending_yaw = 0.0;
            g_pending_pitch = 0.0;
        }
        g_pending_yaw.fetch_add(yaw);
        g_pending_pitch.fetch_add(pitch);
        g_pending_at = now;
        return true;
    }

    CameraFrame read() override {
        CameraFrame f;
        const ULONGLONG now = GetTickCount64();
        int running = 0, active = 0;
        for (Seen& s : g_seen) {
            const uintptr_t self = s.self.load(std::memory_order_relaxed);
            const bool recent = self && now - s.last.load(std::memory_order_relaxed) <= kRecentMs;
            const int32_t a = s.active.load(std::memory_order_relaxed);
            const int32_t r = s.reference.load(std::memory_order_relaxed);
            const uintptr_t iface = s.iface.load(std::memory_order_relaxed);
            if (recent) {
                ++running;
                if (self == g_newest.load(std::memory_order_relaxed)) ++active;
                f.target += std::format("{}0x{:X}", f.target.empty() ? "" : " ", self);
            }
            // Log each object's start and every change of its state, and when it stops running.
            const uintptr_t shown = recent ? self : 0;
            if (shown != s.logged_self || (recent && (a != s.logged_active || r != s.logged_reference ||
                                                      iface != s.logged_iface))) {
                if (recent)
                    log("fb: Dead Space: behaviour 0x%llX runs (thread %u, %u calls): active %s, interface 0x%llX (%s)",
                        (unsigned long long)self, s.thread.load(), s.calls.load(),
                        a == 1 ? "yes" : a == 0 ? "no" : "unknown (cache not valid)", (unsigned long long)iface,
                        reference_name(r));
                else if (s.logged_self)
                    log("fb: Dead Space: behaviour 0x%llX stopped (%u calls)", (unsigned long long)s.logged_self,
                        s.calls.load());
                s.logged_self = shown;
                s.logged_active = a;
                s.logged_reference = r;
                s.logged_iface = iface;
            }
        }
        f.in_game = running > 0;
        f.active = active > 0;
        if (f.target.empty()) f.target = hooked_ ? "no camera update running" : "not hooked";
        return f;
    }

    // The newest behaviour's rotation interface (its yaw is at +0x2C4).
    uintptr_t watch_address() const override {
        const uintptr_t newest = g_newest.load(std::memory_order_relaxed);
        for (const Seen& s : g_seen)
            if (newest && s.self.load(std::memory_order_relaxed) == newest) return s.iface.load(std::memory_order_relaxed);
        return 0;
    }

    std::string status() const override {
        static constexpr const char* kHook[] = {"not yet", "installing", "hooked", "failed"};
        return std::format("camera: Dead Space, {}; camera updates {}, input updates {}, player slot {} ({} calls, {} "
                           "with the player's state), gyro level writes {} ({} capped), input state 0x{:X}",
                           hooked_ ? "hooked" : "not hooked", g_calls.load(), g_input_calls.load(),
                           kHook[g_player_hook.load()], g_player_calls.load(), g_state_calls.load(),
                           g_level_writes.load(), g_capped.load(), g_state.load());
    }

private:
    // HybridRotationInputChimeraBehavior's vtable and update, from its TypeInfo.
    bool resolve_behaviour(const TypeTable& types, std::string& why) {
        const TypeDesc* type = types.find(kBehaviour);
        if (!type) {
            why = std::format("no {} type", kBehaviour);
            return false;
        }
        const std::vector<uintptr_t> gettype = find_gettype(type->type);
        if (gettype.size() != 1) {
            why = std::format("{} getType() found {} times", kBehaviour, gettype.size());
            return false;
        }
        const std::vector<uintptr_t> slots = find_in_image(gettype);
        log("fb: Dead Space: %s TypeInfo %s, getType %s, in %zu vtable slots", kBehaviour,
            mem::module_offset(type->type).c_str(), mem::module_offset(gettype[0]).c_str(), slots.size());
        // The vtable: the slot holding getType whose update slot is code (other hits are data that happens to match).
        for (uintptr_t vtable : slots) {
            uintptr_t update = 0;
            if (!mem::rd(vtable + 8 * kUpdateSlot, update) || !executable(update)) continue;
            uint8_t code[16] = {};
            const bool read = mem::guarded_copy(code, update, sizeof code);
            const bool match = read && std::memcmp(code + kUpdateCheckAt, kUpdateCheck, sizeof kUpdateCheck) == 0;
            log("fb: Dead Space: vtable %s, update (slot %d) %s: %s (%s)", mem::module_offset(vtable).c_str(), kUpdateSlot,
                mem::module_offset(update).c_str(), read ? hex_bytes(code, sizeof code).c_str() : "unreadable",
                match ? "the expected code" : "NOT the expected code");
            if (!match) continue;
            vtable_ = vtable;
            update_ = update;
            return true;
        }
        why = std::format("no {} vtable with the expected update", kBehaviour);
        return false;
    }

    struct Delegate {
        uintptr_t invoker = 0, slot = 0;
    };

    // The delegate invoker whose jump target has the given code, and the one writable place in the exe holding it.
    static bool find_delegate(const char* name, bool (*match)(uintptr_t target), Delegate& out, std::string& why) {
        const std::vector<uintptr_t> invokers = find_pattern(kInvoker, sizeof kInvoker);
        std::vector<uintptr_t> matches;
        uintptr_t target = 0;
        for (uintptr_t invoker : invokers) {
            int32_t rel = 0;
            if (mem::rd(invoker + sizeof kInvoker, rel) && match(invoker + kInvokerSize + rel)) {
                matches.push_back(invoker);
                target = invoker + kInvokerSize + rel;
            }
        }
        if (matches.size() != 1) {
            why = std::format("{} delegate invokers, {} to the {} (1 expected)", invokers.size(), matches.size(), name);
            return false;
        }
        std::vector<uintptr_t> slots = find_in_image(matches);
        std::erase_if(slots, [](uintptr_t s) { return !writable(s); });
        if (slots.size() != 1) {
            why = std::format("the {}'s invoker {} is held in {} writable places (1 expected)", name,
                              mem::module_offset(matches[0]), slots.size());
            return false;
        }
        log("fb: Dead Space: %s %s, invoker %s (one of %zu), held at %s", name, mem::module_offset(target).c_str(),
            mem::module_offset(matches[0]).c_str(), invokers.size(), mem::module_offset(slots[0]).c_str());
        out = {matches[0], slots[0]};
        return true;
    }

    bool resolve_input(std::string& why) {
        return find_delegate(
            "player's input update",
            [](uintptr_t t) { return code_at(t + kInputUpdateCheckAt, kInputUpdateCheck, sizeof kInputUpdateCheck); },
            input_, why);
    }

    uintptr_t vtable_ = 0, update_ = 0;
    Delegate input_;
    bool hooked_ = false;
};

}  // namespace

std::unique_ptr<Camera> make_deadspace_camera() { return std::make_unique<DeadSpaceCamera>(); }

}  // namespace gyro4all::fb
