// Source (Valve; Respawn's branch): Titanfall 2 first. The game is detected by r2\gameinfo.txt and the engine's
// bin\x64_retail\engine.dll. Its exe is only an EA/Origin OOA stub (encrypted, one import from Core\Activation64.dll)
// that loads bin\x64_retail\launcher.dll and the engine's DLLs, which import PeekMessageW (engine.dll) themselves: the
// exe-only import scan can't reach them, so the core hooks the exports inline. inputsystem.dll loads XInput1_3.dll at
// run time. The proxy itself must be in bin\x64_retail (launcher.dll loads the engine with LOAD_WITH_ALTERED_SEARCH_PATH).
//
// The view: the engine client interface (engine.dll's CreateInterface("VEngineClient013")) has the local player's view
// angles behind GetViewAngles / SetViewAngles, vtable slots 63 and 64 in Titanfall 2 v2.0.11.0 (MODLOG 2026-10-21).
// They are the client state's QAngle (floats, degrees, Source's senses: pitch positive looking down, yaw positive
// turning left); SetViewAngles rejects NaN/infinite angles and normalizes each one. Both functions are checked by their
// first bytes before use, so another build is left alone (gyro as right stick).
//
// Writing them from the core's frame doesn't stay: client.dll rebuilds the world angles every frame from the player's
// own (local) angles, and only its mouse step (client.dll+0x24F7A6: GetViewAngles, to local, + the mouse's delta, to
// world, SetViewAngles) feeds the change back. So SetViewAngles is hooked, and the gyro's turn (apply, accumulated) is
// added to the angles that step sets, as more mouse input. The step's call is found by its return address
// (client.dll+0x24F90C), checked by the call's bytes.
#include "source.hpp"

#include "engine.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace gyro4all::source {

namespace {

// Titanfall 2's pitch range (its view stops near straight up/down; the gyro never pushes past it).
constexpr float kMinPitch = -89.0f, kMaxPitch = 89.0f;
constexpr int kGetViewAngles = 63, kSetViewAngles = 64;
// Their first bytes: push rbx / sub rsp,20h / or ecx,-1 / mov rbx,rdx / call <local client state> (GetViewAngles);
// mov [rsp+8],rbx / push rdi / sub rsp,40h / or ecx,-1 (SetViewAngles). Both get the local client state from
// engine.dll+0x78210 (-1 = the active split-screen slot), which the gate reads too.
constexpr uint8_t kGetCode[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xC9, 0xFF, 0x48, 0x8B, 0xDA, 0xE8};
constexpr uint8_t kSetCode[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x83, 0xC9, 0xFF};
// The client state's paused flag (byte; 1 in the pause menu, which pauses the campaign's local server).
constexpr uintptr_t kPaused = 0x160;
// client.dll's mouse step: the return address of its SetViewAngles call, preceded by `call qword ptr [rax+200h]`.
constexpr uintptr_t kMouseSet = 0x24F90C;
constexpr uint8_t kMouseCall[] = {0xFF, 0x90, 0x00, 0x02, 0x00, 0x00};

struct QAngle {
    float pitch, yaw, roll;
};
using GetViewAngles_t = void (*)(void* self, QAngle* out);
using SetViewAngles_t = void (*)(void* self, const QAngle* in);
using CreateInterface_t = void* (*)(const char* name, int* status);
using LocalClient_t = uintptr_t (*)(int slot);

enum class Stage { Waiting, Ready, Failed };
Stage g_stage = Stage::Waiting;
std::string g_why = "engine.dll not loaded yet";
void* g_client = nullptr;
GetViewAngles_t g_get = nullptr;
SetViewAngles_t g_set = nullptr;  // the original (trampoline)
LocalClient_t g_local_client = nullptr;
uintptr_t g_mouse_set = 0;  // client.dll's mouse step call's return address (0 = not found yet)
QAngle g_last{};
bool g_paused = false;

// Game thread: the gyro's turn not yet given to the mouse step (Source's senses), and the step's calls since the last
// status line (the gyro's turns among them).
double g_pending_pitch = 0, g_pending_yaw = 0;
uint32_t g_mouse_calls = 0, g_mouse_turns = 0;
ULONGLONG g_mouse_last = 0;  // GetTickCount64 of the step's last call
// The step not called for this long (pause, menus, loading): a pending turn is dropped, not kept for later. Not "since
// the last frame": the core's frame runs from PeekMessageW, several times per game frame.
constexpr ULONGLONG kMouseIdleMs = 100;

bool code_is(uintptr_t function, const uint8_t* code, size_t n) {
    uint8_t bytes[16] = {};
    return mem::guarded_copy(bytes, function, n) && std::memcmp(bytes, code, n) == 0;
}

void hk_set(void* self, const QAngle* in) {
    if (in && g_mouse_set && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_mouse_set) {
        ++g_mouse_calls;
        g_mouse_last = GetTickCount64();
        if (g_pending_pitch != 0 || g_pending_yaw != 0) {
            QAngle a = *in;
            a.pitch = std::clamp(float(a.pitch + g_pending_pitch), kMinPitch, kMaxPitch);
            a.yaw = float(a.yaw + g_pending_yaw);
            g_pending_pitch = g_pending_yaw = 0;
            ++g_mouse_turns;
            g_set(self, &a);
            return;
        }
    }
    g_set(self, in);
}

// client.dll's mouse step, once client.dll is loaded (checked by the call before the return address).
void find_mouse_step() {
    HMODULE client = GetModuleHandleW(L"client.dll");
    if (!client) return;
    const uintptr_t ret = reinterpret_cast<uintptr_t>(client) + kMouseSet;
    if (!code_is(ret - sizeof kMouseCall, kMouseCall, sizeof kMouseCall)) {
        g_stage = Stage::Failed;
        g_why = "client.dll's mouse step isn't at +0x24F90C (another build?)";
        log("source: %s; the gyro goes to the game as right-stick input", g_why.c_str());
        return;
    }
    g_mouse_set = ret;
    log("source: client.dll's mouse step at %s: the gyro turns through it", mem::module_offset(ret).c_str());
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void fail(std::string why) {
    g_stage = Stage::Failed;
    g_why = std::move(why);
    log("source: %s; the gyro goes to the game as right-stick input", g_why.c_str());
}

// Game thread: find the engine client interface, check its view-angle functions and hook SetViewAngles (once
// engine.dll is loaded).
void resolve() {
    HMODULE engine = GetModuleHandleW(L"engine.dll");
    if (!engine) return;
    auto create = reinterpret_cast<CreateInterface_t>(GetProcAddress(engine, "CreateInterface"));
    if (!create) return fail("engine.dll exports no CreateInterface");
    void* client = create("VEngineClient013", nullptr);
    if (!client) return fail("no VEngineClient013 interface");
    const uintptr_t vtable = mem::rdp(reinterpret_cast<uintptr_t>(client));
    const uintptr_t get = vtable ? mem::rdp(vtable + 8 * kGetViewAngles) : 0;
    const uintptr_t set = vtable ? mem::rdp(vtable + 8 * kSetViewAngles) : 0;
    if (!code_is(get, kGetCode, sizeof kGetCode) || !code_is(set, kSetCode, sizeof kSetCode))
        return fail(std::format("VEngineClient013 slots {}/{} ({} / {}) aren't GetViewAngles/SetViewAngles (another "
                                "build?)",
                                kGetViewAngles, kSetViewAngles, mem::module_offset(get), mem::module_offset(set)));
    int32_t rel = 0;
    if (!mem::rd(get + sizeof kGetCode, rel)) return fail("can't read GetViewAngles' call");
    const uintptr_t local_client = get + sizeof kGetCode + 4 + rel;
    if (!hooks::hook_function(reinterpret_cast<void*>(set), reinterpret_cast<void*>(&hk_set),
                              reinterpret_cast<void**>(&g_set)))
        return fail("can't hook SetViewAngles");
    g_client = client;
    g_get = reinterpret_cast<GetViewAngles_t>(get);
    g_local_client = reinterpret_cast<LocalClient_t>(local_client);
    g_stage = Stage::Ready;
    g_why.clear();
    log("source: VEngineClient013 at %s, GetViewAngles %s, SetViewAngles %s (hooked), local client state %s",
        mem::module_offset(uintptr_t(client)).c_str(), mem::module_offset(get).c_str(), mem::module_offset(set).c_str(),
        mem::module_offset(local_client).c_str());
}

bool finite(const QAngle& a) { return std::isfinite(a.pitch) && std::isfinite(a.yaw) && std::isfinite(a.roll); }

class SourceEngine final : public Engine {
public:
    const char* name() const override { return "Source"; }

    // Respawn's layout: r2\gameinfo.txt and bin\x64_retail\engine.dll next to the exe.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        return exists(dir + L"\\r2\\gameinfo.txt") && exists(dir + L"\\bin\\x64_retail\\engine.dll");
    }

    bool inline_hooks() const override { return true; }

    void configure() override {}

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // The default view while the mouse step runs (gameplay); paused by the client state's flag. Focus and cursor gate
    // the gyro too.
    EngineFrame read_frame() override {
        if (g_stage == Stage::Waiting) resolve();
        if (g_stage == Stage::Ready && !g_mouse_set) find_mouse_step();
        EngineFrame f;
        if (g_stage == Stage::Failed) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        if (g_stage != Stage::Ready || !g_mouse_set) return f;
        QAngle a{};
        g_get(g_client, &a);
        if (!finite(a)) return f;
        g_last = a;
        uint8_t paused = 0;
        const bool was = g_paused;
        g_paused = mem::rd(g_local_client(-1) + kPaused, paused) && paused != 0;
        if (g_paused != was) log("source: %s", g_paused ? "paused" : "unpaused");
        // No mouse step lately: the main menu, loading or the pause (no gameplay view; the stick fallback would take the
        // dropped turn for writes the game undid).
        const bool stepping = GetTickCount64() - g_mouse_last <= kMouseIdleMs;
        if (g_paused || !stepping) g_pending_pitch = g_pending_yaw = 0;
        f.in_game = true;
        f.paused = g_paused;
        f.view_id = stepping ? 1 : 0;
        f.target = "view angles";
        return f;
    }

    // Gyro senses (pitch up, yaw right) to Source's (pitch down, yaw left), for the mouse step's next call.
    bool apply(double pitch, double yaw) override {
        if (g_stage != Stage::Ready || !g_mouse_set) return false;
        g_pending_pitch = std::clamp(g_pending_pitch - pitch, -90.0, 90.0);
        g_pending_yaw = std::clamp(g_pending_yaw - yaw, -180.0, 180.0);
        return true;
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        if (g_stage != Stage::Ready || !g_mouse_set) return false;
        QAngle a{};
        g_get(g_client, &a);
        if (!finite(a)) return false;
        // With the turn the mouse step hasn't taken yet: the stick fallback compares them with what apply was given.
        pitch = -(a.pitch + g_pending_pitch);
        yaw = -(a.yaw + g_pending_yaw);
        camera = reinterpret_cast<uintptr_t>(g_client);
        return true;
    }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (g_stage != Stage::Ready) return "source: " + g_why;
        return std::format("source: view pitch {:.1f} yaw {:.1f}{}, mouse step {} calls ({} with gyro)", g_last.pitch,
                           g_last.yaw, g_paused ? " paused" : "", g_mouse_calls, g_mouse_turns);
    }

    void status_details() override { g_mouse_calls = g_mouse_turns = 0; }
};

}  // namespace

Engine& engine() {
    static SourceEngine instance;
    return instance;
}

}  // namespace gyro4all::source
