// CryEngine (Crytek): Crysis (2007, CryEngine 2), Crysis Remastered, Crysis 2 Remastered and Kingdom Come: Deliverance
// (Warhorse's fork), all 64-bit. The gyro goes the same way in all (Kingdom Come's names differ, below):
// only the local player has a CPlayerInput, whose PreUpdate scales the mouse's delta rotation (+0x64, Ang3) by the
// mouse sensitivity and zoom, adds the controller's, and puts the result into a CMovementRequest on its stack (flags at
// +0, bit 0x40 = has delta rotation, Ang3 radians: x pitch up, z yaw left), which goes to the player's movement
// controller through RequestMovement. The player then clamps the pitch, and handles vehicles and ladders. So
// RequestMovement is hooked, and the gyro's turn (apply, accumulated) is added to the request's delta rotation when the
// call comes from PreUpdate (its return address), after the game's sensitivity and zoom, like more mouse input.
// PreUpdate leaves bit 0x40 clear while the player can't turn: the gyro waits too. Every address is checked by its
// bytes first, so another build is left alone (gyro as right stick). Builds (MODLOG 2026-10-27):
//
// - Crysis, EA app 1.2.1 (6156): the code is in CryGame.dll (fixed image base, no ASLR, MSVC 2005 RTTI). Bin64\
//   Crysis64.exe is a small launcher that loads CrySystem.dll, which imports winmm.dll and version.dll: the proxy loads
//   with it, on the main thread, the game thread. The engine pumps messages with PeekMessageA from CrySystem.dll (the
//   core's hook on the "Cry" modules); CryInput.dll reads the pad through xinput1_3. RequestMovement is
//   CPlayerMovementController's slot 0 (CryGame+0x793B0); the request's delta rotation is at +0x60. Editor.exe
//   (Sandbox 2) and the dedicated server load the same DLLs: plain proxies.
// - Crysis Remastered (Bin64\CrysisRemastered.exe, 2022-02-03 build): one monolithic exe (base 0x140000000, no ASLR,
//   no RTTI), which imports winmm.dll, PeekMessageA and xinput9_1_0. CPlayerInput's vtable is found from its SetState
//   (it logs "CPlayerInput::SetState called: should never happen"), PreUpdate is its slot 1 (exe+0x39E4E0). The
//   movement controller is CPlayer+0x2A0 and RequestMovement its slot 1; with no RTTI to find the controller's vtable,
//   PreUpdate is hooked and the first call reads it from the player. The request's flags are 64-bit and its delta
//   rotation is at +0x88.
// - Crysis 2 Remastered (Bin64\Crysis2Remastered.exe, 2022-08-04 build, data in GameCrysis2\): built like Crysis
//   Remastered (base 0x140000000, no ASLR, no RTTI, the same SetState message). CPlayerInput's vtable exe+0x1C30DD0,
//   PreUpdate its slot 1 (exe+0x12F0D70); CPlayerInput's CPlayer* is at +0x310, the movement controller at
//   CPlayer+0xBB0, RequestMovement its slot 1. PreUpdate adds the delta rotation inline (request+0x78).
// - Kingdom Come: Deliverance 1.9.7 (bin\Win64MasterMasterEpicPGO\KingdomCome.exe, Epic): Warhorse's CryEngine fork.
//   The exe is a small launcher; engine and game are one WHGame.dll (ASLR, RTTI), which imports winmm.dll,
//   version.dll, PeekMessageA/W and xinput9_1_0. The request is Remastered's (64-bit flags, delta rotation at +0x88,
//   set by CMovementRequest::AddDeltaRotation, WHGame+0x21D158). The player's input update (WHGame+0x21D7A8, from
//   C_Player's vtable slot 193) builds it and sends it to the actor's movement controller ([actor]->vtable[0x220/8]);
//   a cold path of the same function (+0xB8D0D9) sends a second one. RequestMovement is
//   C_ActorMovementController's slot 1 (WHGame+0x36CED4, vtable WHGame+0x21C4330, found by its RTTI).
#include "cryengine.hpp"

#include "engine.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>
#include <numbers>
#include <span>

namespace gyro4all::cryengine {

namespace {

struct Build {
    const char* label;
    const wchar_t* module;  // the module with the game code (nullptr = the exe)
    // PreUpdate's request: the return address of its RequestMovement call, preceded by call_code.
    uintptr_t preupdate_call;
    std::span<const uint8_t> call_code;
    uintptr_t delta_rotation;  // the request's delta rotation (Ang3)
    // RequestMovement known in the module: its slot in the movement controller's vtable and the function, checked by
    // its first bytes.
    uintptr_t movement_vtable, request;
    std::span<const uint8_t> request_code;
    // Else found from the player by hooking PreUpdate (CPlayerInput's vtable slot 1, checked by its first bytes): the
    // player's movement controller and RequestMovement's slot in its vtable.
    uintptr_t input_vtable, preupdate;
    std::span<const uint8_t> preupdate_code;
    uintptr_t controller, request_slot;
    // A second call of the player's request (0 = none), checked like the first.
    uintptr_t other_call = 0;
    std::span<const uint8_t> other_code = {};
    uintptr_t input_player = 0x28;  // CPlayerInput's CPlayer* (PreUpdate's search)
};

// Crysis 1.2.1 (6156), CryGame.dll: RequestMovement's mov rax,rsp / push rsi / push rdi / push r12..r14 /
// sub rsp,360h; PreUpdate's lea rdx,[rsp+50h] (the request) / call qword ptr [rax].
constexpr uint8_t kCrysisRequest[] = {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
                                      0x41, 0x56, 0x48, 0x81, 0xEC, 0x60, 0x03, 0x00, 0x00};
constexpr uint8_t kCrysisCall[] = {0x48, 0x8D, 0x54, 0x24, 0x50, 0xFF, 0x10};
// Crysis Remastered, CrysisRemastered.exe: PreUpdate's mov rax,rsp / push rbp / push rbx / push r13 /
// lea rbp,[rax-328h] / sub rsp,410h; its call lea rdx,[rsp+60h] (the request) / mov rcx,[rax+2A0h] (the controller) /
// mov rax,[rcx] / call qword ptr [rax+8].
constexpr uint8_t kRemasteredPreUpdate[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x55, 0x48, 0x8D, 0xA8, 0xD8,
                                            0xFC, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x10, 0x04, 0x00, 0x00};
constexpr uint8_t kRemasteredCall[] = {0x48, 0x8D, 0x54, 0x24, 0x60, 0x48, 0x8B, 0x88, 0xA0,
                                       0x02, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08};

// Kingdom Come: Deliverance 1.9.7, WHGame.dll: RequestMovement's mov [rsp+8],rbx / mov [rsp+10h],rbp / push rsi /
// push rdi / push r14 / sub rsp,30h; the input update's lea rdx,[rsp+70h] (the request) / mov rcx,rax (the controller) /
// mov r8,[rax] / call qword ptr [r8+8], and its cold path's lea rdx,[rbp+110h] / the same.
constexpr uint8_t kKcdRequest[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24,
                                   0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
constexpr uint8_t kKcdCall[] = {0x48, 0x8D, 0x54, 0x24, 0x70, 0x48, 0x8B, 0xC8, 0x4C, 0x8B, 0x00, 0x41, 0xFF, 0x50, 0x08};
constexpr uint8_t kKcdColdCall[] = {0x48, 0x8D, 0x95, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8B,
                                    0xC8, 0x4C, 0x8B, 0x00, 0x41, 0xFF, 0x50, 0x08};

constexpr Build kCrysis{"Crysis 1.2.1 (6156)", L"CryGame.dll", 0x66F29, kCrysisCall, 0x60, 0x3620E8, 0x793B0,
                        kCrysisRequest, 0, 0, {}, 0, 0};
constexpr Build kRemastered{"Crysis Remastered", nullptr, 0x39EBA7, kRemasteredCall, 0x88, 0, 0, {},
                            0x1A56200, 0x39E4E0, kRemasteredPreUpdate, 0x2A0, 1};
// Crysis 2 Remastered, Crysis2Remastered.exe: PreUpdate's mov rax,rsp / push rbp / push rsi / lea rbp,[rax-1D8h] /
// sub rsp,2C8h; its call lea rdx,[rbp-70h] (the request) / mov rcx,[rax+0BB0h] (the controller) / mov rax,[rcx] /
// call qword ptr [rax+8].
constexpr uint8_t kCrysis2RemasteredPreUpdate[] = {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x48, 0x8D, 0xA8, 0x28, 0xFE,
                                                   0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC8, 0x02, 0x00, 0x00};
constexpr uint8_t kCrysis2RemasteredCall[] = {0x48, 0x8D, 0x55, 0x90, 0x48, 0x8B, 0x88, 0xB0, 0x0B,
                                              0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08};
constexpr Build kCrysis2Remastered{"Crysis 2 Remastered", nullptr, 0x12F1737, kCrysis2RemasteredCall, 0x78, 0, 0, {},
                                   0x1C30DD0, 0x12F0D70, kCrysis2RemasteredPreUpdate, 0xBB0, 1, 0, {}, 0x310};
constexpr Build kKcd{"Kingdom Come: Deliverance 1.9.7", L"WHGame.dll", 0x21DD6A, kKcdCall, 0x88, 0x21C4338, 0x36CED4,
                     kKcdRequest, 0, 0, {}, 0, 0, 0xB8D0D9, kKcdColdCall};

constexpr uint32_t kHasDeltaRotation = 0x40;  // in the request's flags (the low dword of Remastered's 64-bit ones)

struct Ang3 {
    float x, y, z;  // x pitch (up), y roll, z yaw (left)
};
using RequestMovement_t = bool (*)(void* self, uint32_t* request);
using PreUpdate_t = void (*)(void* self);

enum class Stage { Waiting, Finding, Ready, Failed };
Stage g_stage = Stage::Waiting;
std::string g_why = "the game's code isn't loaded yet";
const Build* g_build = nullptr;
uintptr_t g_base = 0;                   // the module with the game code
RequestMovement_t g_request = nullptr;  // the original (trampoline)
PreUpdate_t g_preupdate = nullptr;      // the original (trampoline), Remastered
uintptr_t g_preupdate_call = 0;         // PreUpdate's call's return address
uintptr_t g_other_call = 0;             // the second call's (Build::other_call), or 0
bool g_find_logged = false;

// Game thread: the gyro's turn not yet given to the player (radians, CryEngine's senses), and PreUpdate's requests
// since the last status line (the gyro's turns among them).
double g_pending_pitch = 0, g_pending_yaw = 0;
uint32_t g_requests = 0, g_turns = 0;
ULONGLONG g_request_last = 0;  // GetTickCount64 of PreUpdate's last request
// No request for this long (pause, menus, loading): a pending turn is dropped, not kept for later. Not "since the last
// frame": the core's frame runs from PeekMessageA, several times per game frame.
constexpr ULONGLONG kIdleMs = 100;
constexpr double kDegrees = std::numbers::pi / 180.0;

bool code_is(uintptr_t address, std::span<const uint8_t> code) {
    uint8_t bytes[32] = {};
    return code.size() <= sizeof bytes && mem::guarded_copy(bytes, address, code.size()) &&
           std::memcmp(bytes, code.data(), code.size()) == 0;
}

bool hk_request(void* self, uint32_t* request) {
    const auto from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (request && (from == g_preupdate_call || (g_other_call && from == g_other_call))) {
        ++g_requests;
        g_request_last = GetTickCount64();
        if ((*request & kHasDeltaRotation) && (g_pending_pitch != 0 || g_pending_yaw != 0)) {
            auto* delta = reinterpret_cast<Ang3*>(reinterpret_cast<uint8_t*>(request) + g_build->delta_rotation);
            delta->x = float(delta->x + g_pending_pitch);
            delta->z = float(delta->z + g_pending_yaw);
            g_pending_pitch = g_pending_yaw = 0;
            ++g_turns;
        }
    }
    return g_request(self, request);
}

void fail(std::string why) {
    g_stage = Stage::Failed;
    g_why = std::move(why);
    log("cryengine: %s; the gyro goes to the game as right-stick input", g_why.c_str());
}

void hook_request(uintptr_t request) {
    if (!hooks::hook_function(reinterpret_cast<void*>(request), reinterpret_cast<void*>(&hk_request),
                              reinterpret_cast<void**>(&g_request)))
        return fail("can't hook RequestMovement");
    g_stage = Stage::Ready;
    g_why.clear();
    log("cryengine: %s: RequestMovement at %s (hooked), PreUpdate's call returns to %s: the gyro turns through it",
        g_build->label, mem::module_offset(request).c_str(), mem::module_offset(g_preupdate_call).c_str());
}

// Remastered, the game thread: the player's movement controller's RequestMovement, from CPlayerInput (self), in the
// game's code. Until the player has one, the next PreUpdate tries again.
void hk_preupdate(void* self) {
    if (g_stage == Stage::Finding) {
        const uintptr_t player = mem::rdp(reinterpret_cast<uintptr_t>(self) + g_build->input_player);
        const uintptr_t controller = player ? mem::rdp(player + g_build->controller) : 0;
        const uintptr_t vtable = controller ? mem::rdp(controller) : 0;
        const uintptr_t request = vtable ? mem::rdp(vtable + 8 * g_build->request_slot, 1) : 0;
        HMODULE owner = nullptr;
        if (request && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                          reinterpret_cast<LPCWSTR>(request), &owner) &&
            reinterpret_cast<uintptr_t>(owner) == g_base)
            hook_request(request);
        else if (!g_find_logged) {
            g_find_logged = true;
            log("cryengine: the player has no movement controller yet (player %s, controller %s); trying again",
                mem::module_offset(player).c_str(), mem::module_offset(controller).c_str());
        }
    }
    g_preupdate(self);
}

// Game thread: pick the build, check its addresses and hook it (once the module with the game code is loaded).
void resolve() {
    if (!g_build) return fail("no camera hook known for this game yet");
    HMODULE module = g_build->module ? GetModuleHandleW(g_build->module) : GetModuleHandleW(nullptr);
    if (!module) return;
    g_base = reinterpret_cast<uintptr_t>(module);
    const uintptr_t call = g_base + g_build->preupdate_call;
    if (!code_is(call - g_build->call_code.size(), g_build->call_code))
        return fail(std::format("CPlayerInput::PreUpdate's request isn't at {} (not {}?)", mem::module_offset(call),
                                g_build->label));
    g_preupdate_call = call;
    if (g_build->other_call) {
        const uintptr_t other = g_base + g_build->other_call;
        if (!code_is(other - g_build->other_code.size(), g_build->other_code))
            return fail(std::format("the player's second request isn't at {} (not {}?)", mem::module_offset(other),
                                    g_build->label));
        g_other_call = other;
    }
    if (g_build->request) {
        const uintptr_t request = g_base + g_build->request;
        if (mem::rdp(g_base + g_build->movement_vtable, 1) != request || !code_is(request, g_build->request_code))
            return fail(std::format("CPlayerMovementController::RequestMovement isn't at {} (not {}?)",
                                    mem::module_offset(request), g_build->label));
        return hook_request(request);
    }
    const uintptr_t preupdate = g_base + g_build->preupdate;
    if (mem::rdp(g_base + g_build->input_vtable + 8, 1) != preupdate || !code_is(preupdate, g_build->preupdate_code))
        return fail(std::format("CPlayerInput::PreUpdate isn't at {} (not {}?)", mem::module_offset(preupdate),
                                g_build->label));
    if (!hooks::hook_function(reinterpret_cast<void*>(preupdate), reinterpret_cast<void*>(&hk_preupdate),
                              reinterpret_cast<void**>(&g_preupdate)))
        return fail("can't hook CPlayerInput::PreUpdate");
    g_stage = Stage::Finding;
    g_why = "waiting for the player's movement controller";
    log("cryengine: %s: PreUpdate at %s (hooked): RequestMovement comes from the player", g_build->label,
        mem::module_offset(preupdate).c_str());
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = wchar_t(std::towlower(c));
    return s;
}

std::wstring exe_name(const std::wstring& exe_path) { return lower(exe_path.substr(exe_path.find_last_of(L"\\/") + 1)); }

// The Crysis Remastered games: one monolithic exe in Bin64 (it pumps the messages and imports the pad), and the game's
// data next to it. build: nullptr = detected, but no camera hook known yet (the gyro goes to the game as right stick).
struct Remastered {
    const wchar_t* exe;   // lower case
    const wchar_t* data;  // a pak, relative to the exe's folder
    const Build* build;
};
constexpr Remastered kRemasteredGames[] = {
    {L"crysisremastered.exe", L"..\\Game\\GameData.pak", &kRemastered},
    {L"crysis2remastered.exe", L"..\\GameCrysis2\\gamedata_chk0.pak", &kCrysis2Remastered},
};

const Remastered* remastered(const std::wstring& exe_path) {
    const std::wstring name = exe_name(exe_path);
    for (const Remastered& r : kRemasteredGames)
        if (name == r.exe) return &r;
    return nullptr;
}

bool is_remastered(const std::wstring& exe_path) { return remastered(exe_path) != nullptr; }

bool is_kcd(const std::wstring& exe_path) { return exe_name(exe_path) == L"kingdomcome.exe"; }

class CryEngine final : public Engine {
public:
    const char* name() const override { return "CryEngine"; }

    // Bin64 next to Game\: Crysis's engine DLLs and the game's data. A Remastered game: its exe and its data
    // (kRemasteredGames). Kingdom Come: Deliverance: its exe next to WHGame.dll.
    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        if (is_kcd(exe_path)) return exists(dir + L"\\WHGame.dll");
        if (const Remastered* r = remastered(exe_path)) return exists(dir + L"\\" + r->data);
        return exists(dir + L"\\..\\Game\\GameData.pak") && exists(dir + L"\\CrySystem.dll") &&
               exists(dir + L"\\CryGame.dll");
    }

    // Crysis: CrySystem.dll and the renderers pump messages, CryInput.dll reads the pad. Remastered: the exe does.
    // Kingdom Come: WHGame.dll does it all.
    std::wstring module_prefix(const std::wstring& exe_stem) const override {
        const std::wstring stem = lower(exe_stem);
        if (stem == L"kingdomcome") return L"whgame";
        return is_remastered(stem + L".exe") ? L"" : L"cry";
    }

    // The Sandbox editor and the dedicated server load Crysis's DLLs (and the proxy): no gyro there.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring name = exe_name(exe_path);
        return name == L"editor.exe" || name == L"crysisdedicatedserver.exe";
    }

    void configure() override {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const Remastered* r = remastered(exe);
        g_build = is_kcd(exe) ? &kKcd : r ? r->build : &kCrysis;
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    // The default view while PreUpdate requests movement (gameplay, the local player exists); none in menus, the pause
    // and loading, when it doesn't run. Focus and cursor gate the gyro too.
    EngineFrame read_frame() override {
        if (g_stage == Stage::Waiting) resolve();
        EngineFrame f;
        if (g_stage == Stage::Failed) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        if (g_stage != Stage::Ready) return f;
        const bool stepping = GetTickCount64() - g_request_last <= kIdleMs;
        if (!stepping) g_pending_pitch = g_pending_yaw = 0;
        f.in_game = stepping;
        f.view_id = stepping ? 1 : 0;
        f.target = "player input";
        return f;
    }

    // Gyro senses (pitch up, yaw right, degrees) to CryEngine's (pitch up, yaw left, radians), for PreUpdate's next
    // request.
    bool apply(double pitch, double yaw) override {
        if (g_stage != Stage::Ready) return false;
        g_pending_pitch = std::clamp(g_pending_pitch + pitch * kDegrees, -std::numbers::pi / 2, std::numbers::pi / 2);
        g_pending_yaw = std::clamp(g_pending_yaw - yaw * kDegrees, -std::numbers::pi, std::numbers::pi);
        return true;
    }

    bool sees_menus() const override { return false; }

    std::string status_scene() const override {
        if (g_stage != Stage::Ready) return "cryengine: " + g_why;
        return std::format("cryengine: player input {} requests ({} with gyro)", g_requests, g_turns);
    }

    void status_details() override { g_requests = g_turns = 0; }
};

}  // namespace

Engine& engine() {
    static CryEngine instance;
    return instance;
}

}  // namespace gyro4all::cryengine
