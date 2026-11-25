// Risen (2009; the 2023 x64 release, Steam; MODLOG 2026-11-25). <root>\bin\Risen.exe with the engine in DLLs of its own
// (Engine.dll, Game.dll, GUI2.dll, SharedBase.dll...), data in <root>\data\compiled. D3D9; the controller through XInput
// 1.4 (XInputGetState by ordinal, from Engine.dll); SharedBase.dll imports VERSION.dll: Gyro4All goes into bin as
// version.dll. Game.dll and Engine.dll export their C++ names (about 10,800 and 14,800): every function below is found
// by its name, and the few offsets the route needs are read from those functions' own code, each checked.
//
// The view (camtrace's guided search, then the code): the camera follows the player from behind; its yaw is the
// player's own rotation, its pitch an elevation angle of the camera admin. The engine camera's (eCCameraBase) pitch and
// yaw are derived from the camera solver's look direction every frame: not sources (a write there froze the game once).
//
// Input. gCActionAdmin::ProcessBegin (the input module's processor, once a frame, main thread) turns each raw axis into an
// action (gSAction: id, state 4 = an axis value, value): the right stick's X TurnRight (8) / TurnLeft (7), its Y TurnUp
// (9) / TurnDown (10), the value positive (deflection x camera sensitivity x frame time). Each goes through
// SendActionEvent: first to gCCameraAdmin::OnAction (TurnDown: elevation += value x 0.075 degrees, TurnUp -=, clamped to
// -65..75, + looks down; only when the camera type allows pitch), then to the active key script (menus, dialogs), and
// when that doesn't take it, to gCCharacterControl::OnAction (TurnRight stores the value at +0x14, TurnLeft its negation,
// when the camera type allows the character's rotation). Then ProcessBegin calls gCCharacterControl::ProcessMovements
// (only when the input isn't blocked and no GUI holds it), which turns the player by +0x14 x (mouse SensitivityX x 0.003 +
// 0.002) radians (x -1 with InvertX), averaged over the last five frames (weights 1, 0.666, 0.444, 0.295, 0.197: nothing
// lost, about a frame late), and clears it.
// The gyro goes in the same way. ProcessMovements is hooked; before it runs, the gyro's yaw and pitch queued since its
// last call are sent through SendActionEvent as the same axis actions, so the game's own routing decides (key scripts take
// them, camera types that lock the camera ignore them). The character's +0x14 is a store, not a sum: when the gyro's yaw
// reaches it, the stick's value from this frame is added back. The yaw in units: radians / the turn scale above (the
// sensitivity and invert getters are called as ProcessMovements calls them, so the game's mouse settings don't change
// the gyro); the pitch: degrees / 0.075.
//
// Small turns. The movement turns the player toward that look-at in gCCharacterMovement_PS::
// ProcessControledRotationLookAt: the angle is bCVector::GetAngleUnitDeg (SharedBase.dll) of the current direction and the
// look-at, stored at +0x184; under 1 degree the player is rotated by exactly that (CalcNewRotationAngleAndVelocity only
// eases bigger turns). GetAngleUnitDeg is acosf of a float dot: near 0 it only takes the values 0.0198, 0.028, 0.034 ...
// 0.056 degrees (the dot's float steps below 1), so a turn under about 0.014 degrees a frame (2 deg/s at 144 fps) was lost
// and the next ones were rounded down (the gyro alone reached 92% of its turn at 0.05 degrees a frame, nothing at 0.015;
// the stick's bigger turns weren't affected). Game.dll's import of GetAngleUnitDeg is hooked, and the call from there
// (its return address, read from the code) gets the precise angle: atan2 of the cross and dot products. Every other caller
// gets the game's own result.
//
// Menus: the input mode (gCActionAdmin +0x3E0, written by its Set<Mode>ModeActionMap functions: 1 the pause menu, 2
// normal, 3 inventory, ... 15 melee, 16 bow, 17 crossbow, 18 magic; each value read from its setter's code); the gyro runs
// only in the gameplay modes. ProcessMovements stops too in the pause menu and while loading (no call for 250 ms).
//
// Views: the mode also picks the gyro's view. Drawing a ranged weapon or a spell is the game's own toggle (the mode stays
// until it is put away): "Ranged weapon" (bow, crossbow) and "Magic" (magic, cast), which inherits from it, so a player
// who sets Default's Activation Mode to Off and Ranged weapon's to Always on has the gyro only with a bow, crossbow or
// spell out (MODLOG 2026-11-25).
#include "genome_risen1.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "rtti.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <intrin.h>
#include <string>
#include <vector>

namespace gyro4all::genome::risen1 {

namespace {

constexpr double kPi = 3.14159265358979323846;
// ProcessMovements counts as stopped (menus, pause, loading) after this long without a call.
constexpr ULONGLONG kStoppedMs = 250;

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// gSAction as the input module builds it (16 bytes, copied as one): the action, its state, a field axis actions leave 0,
// the value.
struct Action {
    int32_t id;
    int32_t state;
    int32_t unused;
    float value;
};
static_assert(sizeof(Action) == 16);
constexpr int32_t kAxisState = 4;
// gEAction's turn actions. The stick sends positive values; the direction is in the id.
constexpr int32_t kTurnLeft = 7, kTurnRight = 8, kTurnUp = 9, kTurnDown = 10;

// The input modes (gCActionAdmin's action maps), by their setters; view 0: the gyro is off.
struct ModeSetter {
    const char* name;
    const char* setter;
    uint32_t view;
};
constexpr ModeSetter kModeSetters[] = {
    {"pause menu", "?SetMenuModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"normal", "?SetNormalModeActionMap@gCActionAdmin@@QEAAXXZ", 1},
    {"inventory", "?SetInventoryModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"character", "?SetCharacterModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"log", "?SetLogModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"map", "?SetMapModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"loot", "?SetLootModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"pickpocket", "?SetPickPocketModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"item request", "?SetItemRequestModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"craft", "?SetCraftModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"dialog", "?SetDialogModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"quick slot", "?SetQuickSlotModeActionMap@gCActionAdmin@@QEAAX_N@Z", 0},
    {"trade", "?SetTradeModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"lockpick", "?SetLockPickModeActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"melee", "?SetMeleeModeActionMap@gCActionAdmin@@QEAAXXZ", 1},
    {"bow", "?SetBowModeActionMap@gCActionAdmin@@QEAAXXZ", 2},
    {"crossbow", "?SetCrossBowModeActionMap@gCActionAdmin@@QEAAXXZ", 2},
    {"magic", "?SetMagicModeActionMap@gCActionAdmin@@QEAAXXZ", 3},
    {"cast", "?SetCastModeActionMap@gCActionAdmin@@QEAAXXZ", 3},
    {"levitate", "?SetLevitateModeActionMap@gCActionAdmin@@QEAAXXZ", 1},
    {"beast", "?SetBeastModeActionMap@gCActionAdmin@@QEAAXXZ", 1},
    {"nautilus", "?SetNautilusModeActionMap@gCActionAdmin@@QEAAXXZ", 1},
    {"stack amount", "?SetStackAmountModalActionMap@gCActionAdmin@@QEAAXXZ", 0},
    {"accept", "?SetAcceptModalActionMap@gCActionAdmin@@QEAAXXZ", 0},
};

// Game.dll's exports the route uses.
constexpr char kProcessBegin[] = "?ProcessBegin@gCActionAdmin@@UEAAXXZ";
constexpr char kProcessMovements[] = "?ProcessMovements@gCCharacterControl@@QEAAXPEAVeCGeometryEntity@@@Z";
constexpr char kSendActionEvent[] = "?SendActionEvent@gCActionAdmin@@IEAAXAEBUgSAction@@@Z";
constexpr char kCameraOnAction[] = "?OnAction@gCCameraAdmin@@QEAAHAEBUgSAction@@@Z";
constexpr char kCameraName[] = "?GetCurrentCameraName@gCCameraAdmin@@QEAAAEAVbCString@@XZ";
constexpr char kTargetSpace[] = "?GetCameraTargetSpace@gCCameraAdmin@@IEBAAEBVbCMatrix@@XZ";
constexpr char kRotationLookAt[] = "?ProcessControledRotationLookAt@gCCharacterMovement_PS@@IEAAXAEAVbCVector@@@Z";
// SharedBase.dll's.
constexpr char kSharedBase[] = "SharedBase.dll";
constexpr char kAngleUnitDeg[] = "?GetAngleUnitDeg@bCVector@@QEBAMAEBV1@@Z";
// Engine.dll's.
constexpr char kMouseOptions[] = "?GetInstance@eSMouseOptions@@SAAEAU1@XZ";

// The code shapes the offsets are read from.
// SendActionEvent's start: the camera admin's getter (call at +0x15), its result passed to gCCameraAdmin::OnAction (call
// at +0x23, checked against the export).
constexpr char kSendShape[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 8B D6 48 8B C8 48 8B F8 E8 ?? ?? ?? ??";
// gCCameraAdmin::OnAction, the pitch: mulss xmm0, [scale]; movss xmm1, [min]; addss xmm0, [rbx+elevation]; maxss;
// movss xmm0, [max]; minss; movss [rbx+elevation], xmm0.
constexpr char kPitchShape[] =
    "F3 0F 59 05 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? F3 0F 58 83 ?? ?? ?? ?? F3 0F 5F C8 F3 0F 10 05 ?? ?? ?? ?? F3 0F 5D "
    "C1 F3 0F 11 83 ?? ?? ?? ??";
// ProcessMovements, the turn: call [eSMouseOptions::GetInstance]; mov rcx, rax; mov rbx, rax; call <SensitivityX>;
// lea rcx, [rbx+invert]; movaps xmm10, xmm0; call <InvertX>; test al, al; je; movss xmm6, [-1]; mulss xmm10, [mul];
// mov rcx, [rdi+x]; add rcx, x; addss xmm10, [add]; mulss xmm10, xmm6; mulss xmm10, [rdi+turn].
constexpr char kTurnShape[] =
    "FF 15 ?? ?? ?? ?? 48 8B C8 48 8B D8 E8 ?? ?? ?? ?? 48 8D 4B ?? 44 0F 28 D0 E8 ?? ?? ?? ?? 84 C0 74 ?? F3 0F 10 35 "
    "?? ?? ?? ?? F3 44 0F 59 15 ?? ?? ?? ?? 48 8B 4F ?? 48 81 C1 ?? ?? ?? ?? F3 44 0F 58 15 ?? ?? ?? ?? F3 44 0F 59 D6 F3 "
    "44 0F 59 57 ??";
// A getter returning a field's address: lea rax, [rcx+offset]; ret.
constexpr char kFieldGetterShape[] = "48 8D 81 ?? ?? ?? ?? C3";
// ProcessControledRotationLookAt, the turn's angle: lea rdx, [rsp+target]; lea rcx, [rsp+current]; call
// [GetAngleUnitDeg]; lea r8, [rsp+target]; movss [rbx+angle], xmm0.
constexpr char kTurnAngleShape[] =
    "48 8D 54 24 ?? 48 8D 4C 24 ?? FF 15 ?? ?? ?? ?? 4C 8D 44 24 ?? F3 0F 11 83 ?? ?? ?? ??";
// A mode setter's store of its mode: mov dword [rcx+mode], value (the first in the setter).
constexpr char kModeStoreShape[] = "C7 81 ?? ?? ?? ?? ?? ?? ?? ??";

int32_t rd_i32(uintptr_t at) {
    int32_t v = 0;
    mem::rd(at, v);
    return v;
}

float rd_f32(uintptr_t at) {
    float v = 0;
    mem::rd(at, v);
    return v;
}

// The target of a call, or of a rip-relative operand, whose rel32 ends the instruction at `at`.
uintptr_t rel_target(uintptr_t at) { return at ? at + 4 + rd_i32(at) : 0; }

// The first match of `pattern` in [from, from + len), 0 if none.
uintptr_t find_in(uintptr_t from, size_t len, const char* pattern) {
    if (!from) return 0;
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(pattern, bytes, mask);
    const auto hits = mem::find_pattern({mem::Range{from, from + len}}, bytes.data(), mask.data(), bytes.size());
    return hits.empty() ? 0 : hits[0];
}

bool starts_with(uintptr_t at, const char* pattern) { return at && find_in(at, 64, pattern) == at; }

std::string hex(uintptr_t v) { return std::format("0x{:X}", v); }

// --- what the code gives (read once, on the first frame) ---------------------------------------------------------------

using BeginFn = void (*)(void* action_admin);
using MovementsFn = void (*)(void* control, void* entity);
using SendFn = void (*)(void* action_admin, const Action* action);
using CameraAdminFn = void* (*)();
using OptionsFn = void* (*)();
using SensitivityFn = float (*)(void* options);
using InvertFn = bool (*)(void* option);
using AngleFn = float (*)(const float* a, const float* b);

BeginFn g_begin_orig = nullptr;
MovementsFn g_movements_orig = nullptr;
SendFn g_send = nullptr;
CameraAdminFn g_camera_admin = nullptr;
OptionsFn g_mouse_options = nullptr;
SensitivityFn g_sensitivity = nullptr;
InvertFn g_invert = nullptr;
AngleFn g_angle_orig = nullptr;
uintptr_t g_angle_site = 0;             // the return address of ProcessControledRotationLookAt's GetAngleUnitDeg call
uintptr_t g_invert_at = 0;              // eSMouseOptions: InvertX (the invert getter's argument)
double g_turn_mul = 0, g_turn_add = 0;  // radians per turn unit = SensitivityX x mul + add
uintptr_t g_turn = 0;                   // gCCharacterControl: the turn input this frame (float)
double g_pitch_scale = 0;               // degrees of elevation per pitch unit
uintptr_t g_elevation = 0;              // gCCameraAdmin: the elevation (float degrees, + looks down)
float g_pitch_min = 0, g_pitch_max = 0;
uintptr_t g_mode = 0;                   // gCActionAdmin: the input mode
struct ModeValue {
    uint32_t value;
    const ModeSetter* setter;
};
std::vector<ModeValue> g_modes;

const ModeSetter* mode_of(uint32_t value) {
    for (const ModeValue& m : g_modes)
        if (m.value == value) return m.setter;
    return nullptr;
}

// --- the hooks (main thread, inside the game's frame) ----------------------------------------------------------------

std::atomic<uintptr_t> g_action_admin{0};   // gCActionAdmin, from its ProcessBegin
std::atomic<uintptr_t> g_camera{0};         // gCCameraAdmin, from its getter, each ProcessMovements
std::atomic<uint32_t> g_movements{0};       // ProcessMovements calls with a player entity
std::atomic<bool> g_live{false};            // read_frame: a gameplay mode with ProcessMovements running
std::atomic<int64_t> g_yaw_nano{0}, g_pitch_nano{0};          // the gyro's turn queued for the next ProcessMovements
std::atomic<int64_t> g_sent_yaw_nano{0}, g_sent_pitch_nano{0};  // for the status line: sent
std::atomic<uint32_t> g_merged{0}, g_not_taken{0};             // stick turns added back; gyro yaws the character didn't take
std::atomic<uint32_t> g_precise{0};                            // player turns under 1 degree given their precise angle
std::atomic<int64_t> g_recovered_nano{0};                      // and how much that changed them (|precise - game|)

// The turn scale (radians per unit, signed: InvertX), from the game's own getters, at most every half second.
double g_scale = 0;
ULONGLONG g_scale_at = 0;
float g_sensitivity_seen = -1;
bool g_invert_seen = false;

double turn_scale() {
    const ULONGLONG now = GetTickCount64();
    if (g_scale && now - g_scale_at < 500) return g_scale;
    void* options = g_mouse_options ? g_mouse_options() : nullptr;
    if (!options) return 0;
    const float s = g_sensitivity(options);
    const bool invert = g_invert(static_cast<char*>(options) + g_invert_at);
    const double k = (double(s) * g_turn_mul + g_turn_add) * (invert ? -1 : 1);
    if (!std::isfinite(k) || std::fabs(k) < 1e-6 || s < 0 || s > 100) return 0;
    if (s != g_sensitivity_seen || invert != g_invert_seen)
        log("genome: Risen: mouse SensitivityX %.3f%s: gyro yaw sent as %.5f rad a unit", s, invert ? ", InvertX" : "", k);
    g_sensitivity_seen = s;
    g_invert_seen = invert;
    g_scale = k;
    g_scale_at = now;
    return k;
}

void hooked_begin(void* action_admin) {
    g_action_admin.store(reinterpret_cast<uintptr_t>(action_admin), std::memory_order_relaxed);
    g_begin_orig(action_admin);
}

bool gameplay_mode(uintptr_t action_admin) {
    uint32_t mode = 0;
    const ModeSetter* m = mem::rd(action_admin + g_mode, mode) ? mode_of(mode) : nullptr;
    return m && m->view;
}

// The gyro's queued turn, as the game's own axis actions (see the top of the file).
void send_turn(uintptr_t control, double yaw_deg, double pitch_deg) {
    const uintptr_t admin = g_action_admin.load(std::memory_order_relaxed);
    if (!admin || !gameplay_mode(admin)) return;
    if (yaw_deg != 0) {
        if (const double k = turn_scale()) {
            const double units = yaw_deg * kPi / 180 / k;
            const Action a{units >= 0 ? kTurnRight : kTurnLeft, kAxisState, 0, float(std::fabs(units))};
            float before = 0, after = 0;
            if (mem::rd(control + g_turn, before)) {
                g_send(reinterpret_cast<void*>(admin), &a);
                if (mem::rd(control + g_turn, after) && after != before) {
                    if (before != 0) {
                        const float sum = before + after;
                        mem::guarded_store(control + g_turn, &sum, sizeof sum);
                        g_merged.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_sent_yaw_nano.fetch_add(int64_t(yaw_deg * 1e9), std::memory_order_relaxed);
                } else {
                    g_not_taken.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    if (pitch_deg != 0) {
        const double units = -pitch_deg / g_pitch_scale;  // the elevation looks down when it grows; GyroLib's pitch is up
        const Action a{units >= 0 ? kTurnDown : kTurnUp, kAxisState, 0, float(std::fabs(units))};
        g_send(reinterpret_cast<void*>(admin), &a);
        g_sent_pitch_nano.fetch_add(int64_t(pitch_deg * 1e9), std::memory_order_relaxed);
    }
}

// GetAngleUnitDeg (bCVector, this = a): the game's result, except for the player's turn angle (the call in
// ProcessControledRotationLookAt): atan2 of |a x b| and a . b, precise where acosf of the dot rounds (see the top of the
// file). The sign is the game's own (applied after, from the cross product).
float hooked_angle(const float* a, const float* b) {
    const float game = g_angle_orig(a, b);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_angle_site || !a || !b) return game;
    const double c[3] = {double(a[1]) * b[2] - double(a[2]) * b[1], double(a[2]) * b[0] - double(a[0]) * b[2],
                         double(a[0]) * b[1] - double(a[1]) * b[0]};
    const double dot = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    const double deg = std::atan2(std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), dot) * 180 / kPi;
    if (!std::isfinite(deg) || deg >= 1) return game;  // big turns: the game's (eased) path, its own precision is enough
    g_precise.fetch_add(1, std::memory_order_relaxed);
    g_recovered_nano.fetch_add(int64_t(std::fabs(deg - double(game)) * 1e9), std::memory_order_relaxed);
    return float(deg);
}

void hooked_movements(void* control, void* entity) {
    if (control && entity) {
        g_movements.fetch_add(1, std::memory_order_relaxed);
        if (g_camera_admin) g_camera.store(reinterpret_cast<uintptr_t>(g_camera_admin()), std::memory_order_relaxed);
        const int64_t yaw = g_yaw_nano.exchange(0, std::memory_order_acq_rel);
        const int64_t pitch = g_pitch_nano.exchange(0, std::memory_order_acq_rel);
        if ((yaw || pitch) && g_live.load(std::memory_order_relaxed))
            send_turn(reinterpret_cast<uintptr_t>(control), double(yaw) * 1e-9, double(pitch) * 1e-9);
    }
    g_movements_orig(control, entity);
}

// --- reading the code --------------------------------------------------------------------------------------------------

std::string g_missing;  // what the code didn't give, for the log and the status line

bool read_code() {
    const auto note = [](const std::string& what) { g_missing += (g_missing.empty() ? "" : "; ") + what; };
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    // The game's own Game.dll and Engine.dll (in the exe's folder, not any other module of that name).
    const auto own = [&](const wchar_t* name) -> HMODULE {
        HMODULE m = GetModuleHandleW(name);
        wchar_t path[MAX_PATH] = {};
        return m && GetModuleFileNameW(m, path, MAX_PATH) && _wcsicmp(path, (dir + name).c_str()) == 0 ? m : nullptr;
    };
    HMODULE game = own(L"Game.dll"), engine = own(L"Engine.dll");
    if (!game || !engine) {
        note("Game.dll or Engine.dll not loaded from the game's folder");
        return false;
    }
    const auto fn = [](HMODULE m, const char* name) { return reinterpret_cast<uintptr_t>(GetProcAddress(m, name)); };
    const uintptr_t begin = fn(game, kProcessBegin), movements = fn(game, kProcessMovements),
                    send = fn(game, kSendActionEvent), camera_on_action = fn(game, kCameraOnAction),
                    mouse_options = fn(engine, kMouseOptions);
    if (!begin || !movements || !send || !camera_on_action || !mouse_options) {
        note("an exported function is missing (another build?)");
        return false;
    }

    // The camera admin's getter, from SendActionEvent (its result goes to gCCameraAdmin::OnAction).
    if (starts_with(send, kSendShape) && rel_target(send + 0x24) == camera_on_action)
        g_camera_admin = reinterpret_cast<CameraAdminFn>(rel_target(send + 0x16));
    else
        note("SendActionEvent's code shape (the camera admin's getter)");

    // The pitch's scale, limits and the elevation, from gCCameraAdmin::OnAction.
    if (const uintptr_t at = find_in(camera_on_action, 0x800, kPitchShape); at && rd_i32(at + 0x14) == rd_i32(at + 0x2C)) {
        g_pitch_scale = rd_f32(rel_target(at + 4));
        g_pitch_min = rd_f32(rel_target(at + 0xC));
        g_pitch_max = rd_f32(rel_target(at + 0x20));
        g_elevation = uintptr_t(rd_i32(at + 0x14));
    }
    if (!(g_pitch_scale > 0.001 && g_pitch_scale < 10) || !(g_pitch_min < 0 && g_pitch_max > 0 && g_pitch_max <= 90) ||
        !g_elevation || g_elevation > 0x2000) {
        g_pitch_scale = 0;
        g_elevation = 0;
        note("the camera's pitch (gCCameraAdmin::OnAction's code shape): no gyro pitch");
    }

    // The turn's scale, the mouse settings' getters and the character's turn field, from ProcessMovements.
    if (const uintptr_t at = find_in(movements, 0x1000, kTurnShape)) {
        uintptr_t slot = rel_target(at + 2), imported = 0;
        if (mem::rd(slot, imported) && imported == mouse_options) {
            g_mouse_options = reinterpret_cast<OptionsFn>(mouse_options);
            g_sensitivity = reinterpret_cast<SensitivityFn>(rel_target(at + 0xD));
            g_invert = reinterpret_cast<InvertFn>(rel_target(at + 0x1A));
            g_invert_at = *reinterpret_cast<const uint8_t*>(at + 0x14);
            g_turn_mul = rd_f32(rel_target(at + 0x2F));
            g_turn_add = rd_f32(rel_target(at + 0x43));
            g_turn = *reinterpret_cast<const uint8_t*>(at + 0x51);
        }
    }
    if (!g_mouse_options || !(g_turn_mul > 0 && g_turn_mul < 1) || !(g_turn_add >= 0 && g_turn_add < 1) || !g_turn) {
        g_mouse_options = nullptr;
        g_turn = 0;
        note("ProcessMovements' turn (its code shape): no gyro yaw");
    }

    // The input modes, from their setters: the normal mode's names the field, the others must store to it too.
    const auto mode_store = [&](const ModeSetter& s) { return find_in(fn(game, s.setter), 0x60, kModeStoreShape); };
    for (const ModeSetter& s : kModeSetters)
        if (std::strcmp(s.name, "normal") == 0)
            if (const uintptr_t store = mode_store(s); store && rd_i32(store + 2) > 0 && rd_i32(store + 2) < 0x1000)
                g_mode = uintptr_t(rd_i32(store + 2));
    for (const ModeSetter& s : kModeSetters)
        if (const uintptr_t store = g_mode ? mode_store(s) : 0; store && uintptr_t(rd_i32(store + 2)) == g_mode)
            g_modes.push_back({uint32_t(rd_i32(store + 6)), &s});
    if (!g_mode) note("the input modes (the mode setters' code): no gyro");
    if (!g_mode || !g_camera_admin || (!g_turn && !g_elevation)) return false;

    // The player's turn angle: the GetAngleUnitDeg call in ProcessControledRotationLookAt, through Game.dll's import of
    // it (the slot must hold SharedBase.dll's export).
    const HMODULE shared = GetModuleHandleW(L"SharedBase.dll");
    const uintptr_t angle_fn = shared ? fn(shared, kAngleUnitDeg) : 0;
    const uintptr_t call = find_in(fn(game, kRotationLookAt), 0x400, kTurnAngleShape);
    uintptr_t slot_value = 0;
    if (call && angle_fn && mem::rd(rel_target(call + 0xC), slot_value) && slot_value == angle_fn)
        g_angle_site = call + 0x10;
    else
        note("ProcessControledRotationLookAt's code shape: the game's rounding of small turns kept");

    // The hooks: ProcessBegin names the action admin, ProcessMovements takes the gyro's turn.
    if (!hooks::hook_function(reinterpret_cast<void*>(begin), reinterpret_cast<void*>(&hooked_begin),
                              reinterpret_cast<void**>(&g_begin_orig)) ||
        !hooks::hook_function(reinterpret_cast<void*>(movements), reinterpret_cast<void*>(&hooked_movements),
                              reinterpret_cast<void**>(&g_movements_orig))) {
        note("hooking ProcessBegin / ProcessMovements failed");
        return false;
    }
    if (g_angle_site && (!hooks::add(kSharedBase, kAngleUnitDeg, reinterpret_cast<void*>(&hooked_angle),
                                     reinterpret_cast<void**>(&g_angle_orig)) ||
                         !g_angle_orig)) {
        g_angle_site = 0;
        note("hooking GetAngleUnitDeg's import failed: the game's rounding of small turns kept");
    }
    g_send = reinterpret_cast<SendFn>(send);
    return true;
}

// The camera admin's fields for the log and the status line, from its exported getters (0 if not readable).
uintptr_t g_camera_name = 0, g_target_space = 0;

// --- the frame ---------------------------------------------------------------------------------------------------------

bool g_tried = false, g_ready = false;
uint32_t g_last_movements = 0;
ULONGLONG g_last_movement_at = 0;
bool g_player_seen = false;
uint32_t g_last_mode = 0xFFFFFFFF;
int g_mode_logs = 0, g_name_logs = 0;
std::string g_camera_seen;
double g_yaw_rest = 0, g_pitch_rest = 0;  // nano-degrees not queued yet (under 1)
// Status line: the heading (the camera target space's forward) turned in gameplay, and the elevation's change.
double g_turned = 0, g_elevation_moved = 0, g_last_heading = 0;
float g_last_elevation = 0;
bool g_tracking = false;

std::string read_camera_name(uintptr_t camera) {
    const uintptr_t s = g_camera_name ? mem::rdp(camera + g_camera_name, 1) : 0;
    char buf[48] = {};
    if (!s || !mem::guarded_copy(buf, s, sizeof buf - 1)) return {};
    return std::string(buf, strnlen(buf, sizeof buf - 1));
}

bool read_heading(uintptr_t camera, double& deg) {
    float f[3];
    if (!camera || !g_target_space || !mem::rd(camera + g_target_space + 0x20, f)) return false;
    const double len = std::sqrt(double(f[0]) * f[0] + double(f[2]) * f[2]);
    if (!std::isfinite(len) || std::fabs(len - 1) > 0.02) return false;
    deg = std::atan2(f[0], f[2]) * 180 / kPi;
    return true;
}

bool read_elevation(uintptr_t camera, float& e) {
    return camera && g_elevation && mem::rd(camera + g_elevation, e) && std::isfinite(e) && e >= -90 && e <= 90;
}

}  // namespace

bool detect(const std::wstring& exe_path) {
    const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
    const size_t up = dir.find_last_of(L"\\/");
    if (up == std::wstring::npos || _wcsicmp(dir.c_str() + up + 1, L"bin") != 0) return false;
    const std::wstring root = dir.substr(0, up);
    return exists(dir + L"\\Game.dll") && exists(dir + L"\\Engine.dll") && exists(dir + L"\\SharedBase.dll") &&
           exists(root + L"\\data\\compiled") && exists(root + L"\\data\\ini");
}

const wchar_t* module_prefix() { return L"engine.dll|game.dll|gui2.dll"; }

void configure() {
    log("genome: Risen (bin\\ layout, Game.dll exports): the camera route is read from Game.dll on the first frame");
}

// Magic inherits from Ranged weapon: one setting there covers every drawn weapon or spell (see the top of the file).
std::vector<ViewInfo> views() {
    return {{1, "Default", 0, false}, {2, "Ranged weapon", 1, false}, {3, "Magic", 2, false}};
}

EngineFrame read_frame() {
    EngineFrame f;
    if (!g_tried) {
        g_tried = true;
        g_ready = read_code();
        if (g_ready) {
            // The camera admin's name and target space getters (status only).
            if (HMODULE game = GetModuleHandleW(L"Game.dll")) {
                const auto field = [&](const char* name) {
                    const auto at = reinterpret_cast<uintptr_t>(GetProcAddress(game, name));
                    return starts_with(at, kFieldGetterShape) ? uintptr_t(rd_i32(at + 3)) : 0;
                };
                g_camera_name = field(kCameraName);
                g_target_space = field(kTargetSpace);
            }
            std::string modes;
            for (const ModeValue& m : g_modes)
                if (m.setter->view) modes += std::format("{}{} {}", modes.empty() ? "" : ", ", m.setter->name, m.value);
            log("genome: Risen: ProcessBegin and ProcessMovements hooked; gyro as the game's turn actions through "
                "SendActionEvent. Yaw: (SensitivityX x %.4f + %.4f) rad a unit, the character's turn +%s; pitch: %.3f "
                "degrees a unit, elevation +%s (%.0f..%.0f); input mode +%s, gyro in: %s; camera name +%s, target space +%s; "
                "small turns' precise angle %s%s%s",
                g_turn_mul, g_turn_add, hex(g_turn).c_str(), g_pitch_scale, hex(g_elevation).c_str(), g_pitch_min,
                g_pitch_max, hex(g_mode).c_str(), modes.c_str(), hex(g_camera_name).c_str(), hex(g_target_space).c_str(),
                g_angle_site ? ("on (GetAngleUnitDeg's import, the call at " + mem::module_offset(g_angle_site) + ")").c_str()
                             : "off",
                g_missing.empty() ? "" : "; not found: ", g_missing.c_str());
        } else {
            log("genome: Risen: no camera hook for this build (%s): the gyro goes to the game as right stick",
                g_missing.c_str());
        }
    }
    if (!g_ready) {  // a build whose code doesn't match: the gyro as right stick, like the other modules' "no camera hook"
        f.no_camera = true;
        f.in_game = true;
        f.view_id = 1;
        f.target = "no camera hook (gyro as right stick)";
        return f;
    }

    const ULONGLONG now = GetTickCount64();
    const uint32_t calls = g_movements.load(std::memory_order_relaxed);
    if (calls != g_last_movements) {
        g_last_movements = calls;
        g_last_movement_at = now;
        g_player_seen = true;
    }
    const bool moving = g_last_movement_at && now - g_last_movement_at <= kStoppedMs;

    const uintptr_t admin = g_action_admin.load(std::memory_order_relaxed);
    uint32_t mode = 0;
    const bool mode_known = admin && mem::rd(admin + g_mode, mode);
    const ModeSetter* m = mode_known ? mode_of(mode) : nullptr;
    if (mode_known && mode != g_last_mode && g_mode_logs < 300) {
        ++g_mode_logs;
        log("genome: Risen: input mode %u (%s)%s", mode, m ? m->name : "not known", m && m->view ? "" : ": gyro off");
    }
    if (mode_known) g_last_mode = mode;

    const uintptr_t camera = g_camera.load(std::memory_order_relaxed);
    const std::string name = camera ? read_camera_name(camera) : std::string();
    if (name != g_camera_seen && !name.empty() && g_name_logs < 200) {
        ++g_name_logs;
        log("genome: Risen: camera \"%s\"", name.c_str());
    }
    if (!name.empty()) g_camera_seen = name;

    const uint32_t view = m ? m->view : 0;
    const bool live = moving && view;
    g_live.store(live, std::memory_order_relaxed);
    if (!live) {  // no turn queued across menus, pauses or loading
        g_yaw_nano.store(0, std::memory_order_relaxed);
        g_pitch_nano.store(0, std::memory_order_relaxed);
    }
    f.in_game = g_player_seen && mode_known;
    f.paused = !moving;
    f.view_id = live ? view : 0;
    f.target = g_camera_seen.empty() ? "Risen" : g_camera_seen;

    // Status: what the heading and the elevation did in gameplay.
    double h = 0;
    float e = 0;
    if (live && read_heading(camera, h) && read_elevation(camera, e)) {
        if (g_tracking) {
            g_turned += std::remainder(h - g_last_heading, 360.0);
            g_elevation_moved += double(e) - g_last_elevation;
        }
        g_last_heading = h;
        g_last_elevation = e;
        g_tracking = true;
    } else {
        g_tracking = false;
    }
    return f;
}

// Degrees from GyroLib (pitch up, yaw right), queued for the next ProcessMovements.
bool apply(double pitch, double yaw) {
    if (!g_ready || !g_live.load(std::memory_order_relaxed)) return false;
    if (g_turn) {
        g_yaw_rest += yaw * 1e9;
        const int64_t whole = int64_t(g_yaw_rest);
        g_yaw_rest -= double(whole);
        if (whole) g_yaw_nano.fetch_add(whole, std::memory_order_acq_rel);
    }
    if (g_elevation) {
        g_pitch_rest += pitch * 1e9;
        const int64_t whole = int64_t(g_pitch_rest);
        g_pitch_rest -= double(whole);
        if (whole) g_pitch_nano.fetch_add(whole, std::memory_order_acq_rel);
    }
    return true;
}

Engine::Recenter recenter_support(std::string& detail) {
    float e = 0;
    if (!g_ready || !g_elevation) {
        detail = "the camera's elevation isn't known in this build";
        return Engine::Recenter::No;
    }
    if (!g_live.load(std::memory_order_relaxed) || !read_elevation(g_camera.load(std::memory_order_relaxed), e)) {
        detail = "camera not readable";
        return Engine::Recenter::NotYet;
    }
    detail = std::format("{} pitch {:.1f}", g_camera_seen, -e);
    return Engine::Recenter::Yes;
}

// The elevation is the pitch's source (the game adds the stick's pitch to it): written directly, toward level (0).
bool recenter(double fraction) {
    const uintptr_t camera = g_camera.load(std::memory_order_relaxed);
    float e = 0;
    if (!read_elevation(camera, e)) return false;
    const float out = std::clamp(float(e * (1.0 - fraction)), g_pitch_min, g_pitch_max);
    return mem::guarded_store(camera + g_elevation, &out, sizeof out);
}

std::string status_scene() {
    if (!g_ready) return "genome: Risen: no camera hook (gyro as right stick): " + g_missing;
    const uintptr_t camera = g_camera.load(std::memory_order_relaxed);
    const ModeSetter* m = mode_of(g_last_mode);
    double h = 0;
    float e = 0;
    return std::format("genome: Risen, input mode {}, camera \"{}\", heading {} elevation {}",
                       m ? m->name : std::to_string(g_last_mode), g_camera_seen,
                       read_heading(camera, h) ? std::format("{:.1f}", h) : std::string("?"),
                       read_elevation(camera, e) ? std::format("{:.1f}", e) : std::string("?"));
}

// Since the last status line: how far the heading turned and the elevation moved in gameplay, against the gyro's turn
// sent (the rest is the stick's), the stick turns merged with the gyro's, and gyro yaws the character didn't take (a key
// script or a camera type that locks the character's rotation took them).
void status_details() {
    const double yaw = double(g_sent_yaw_nano.exchange(0)) * 1e-9, pitch = double(g_sent_pitch_nano.exchange(0)) * 1e-9;
    log("status: genome: Risen: heading turned %+.1f, gyro yaw %+.1f, rest %+.1f deg; elevation %+.1f, gyro pitch %+.1f "
        "(elevation looks down), rest %+.1f deg; stick turns merged %u, gyro yaws not taken %u; small turns made "
        "precise %u (changed %.2f deg)",
        g_turned, yaw, g_turned - yaw, g_elevation_moved, pitch, g_elevation_moved + pitch, g_merged.exchange(0),
        g_not_taken.exchange(0), g_precise.exchange(0), double(g_recovered_nano.exchange(0)) * 1e-9);
    g_turned = g_elevation_moved = 0;
}

}  // namespace gyro4all::genome::risen1
