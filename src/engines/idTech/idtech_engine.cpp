// id Tech (id Software): DOOM (2016), id Tech 6, first. The game is detected by its resource container next to the exe
// (base\gameresources.resources). Its launcher exe (DOOMx64.exe) relaunches DOOMx64vk.exe for Vulkan, which then owns
// the session (dllmain.cpp), and the game pumps window messages on a thread of its own (the core adopts it).
//
// The scene: id's type info in the exe (idtech_types.hpp) gives the field offsets by name; RTTI gives idGameLocal's
// vtable, and gameLocal (a heap object) starts with it; the local idPlayer is found in the heap by its vtable and its
// embedded physicsObj's (idPhysics_Player), or through gameLocal.playerList. physicsObj holds viewAngles, which follow
// the camera, and current.deltaViewAngles, the offset the game adds to the input's angles (spawns, teleports) and
// otherwise leaves alone (MODLOG 2026-10-19). The gyro's turn is added to deltaViewAngles, in id Tech's senses:
// idAngles are degrees, pitch positive looking down, yaw positive turning left.
// id Tech 5 (Wolfenstein: The New Order) has type info without the player's fields: the offsets are fixed (found with
// WriteWatch in the Game Pass build 1.14.1.0) and checked against the game's own equation on every new player; the input
// generator's angles, at an exe address in that build, are searched for by value in any other.
// MachineGames' id Tech 6 (Wolfenstein II) likewise: no idPlayer or player physics in its type info, a Havok player
// physics (idHavokPhysics_Player, RTTI) embedded in idPlayer, and, as in id Tech 5, the input generator's angles in the
// exe as the view's source (found by turn recordings in the Game Pass build 1.5.0.0; the generator is found by its
// vtable, the physics offset in the player, so other builds and stores need no exe address).
// MachineGames' id Tech 7 (Indiana Jones and the Great Circle): an entity-component game with no idPlayer, idGameLocal
// or player physics at all (type info or RTTI); the input generator's angles are the view's source too, at another
// offset (Game Pass build 1.16.1.0, MODLOG 2026-10-20). The generator stands in for the player (there is none).
#include "idtech.hpp"
#include "idtech_types.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "writewatch.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace gyro4all::idtech {

namespace {

// The view's pitch limits (DOOM's pm_minviewpitch / pm_maxviewpitch defaults are within them): the gyro never pushes
// the view past them, so the turn it adds doesn't pile up in deltaViewAngles beyond what the game shows.
constexpr float kMinPitch = -89.0f, kMaxPitch = 89.0f;
// id Tech 7: the player's game time unchanged this long means paused or loading (a 60 Hz tick changes it every
// 16.7 ms); over kTickMs its view angles aren't reported (view_angles), so the core's stick fallback can't take the
// writes the paused game doesn't apply for undone ones (it did, while the pause menu or the map opened).
constexpr ULONGLONG kFrozenMs = 60, kTickMs = 20;
// limit_turn: how long the view must stay put while turns ask for more pitch to count as the game's limit.
constexpr ULONGLONG kStuckMs = 40;

// Milliseconds from the performance counter (GetTickCount64 moves in ~15.6 ms steps: too coarse for kTickMs).
ULONGLONG precise_ms() {
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return ULONGLONG(now.QuadPart / (freq / 1000));
}

// Field offsets from the type info (-1 = not found). The player's physics object by game: id Tech 6 (DOOM 2016)
// idPlayer.physicsObj (idPhysics_Player); id Tech 7 (DOOM Eternal) idPlayer.physicsObjHavok (idHavokPhysics_Player),
// which also holds a deltaViewAngles of its own and has no idGameLocal; id Tech 8 (The Dark Ages) a pointer,
// idPlayer.idPlayerPhysicsInfo (idPlayerPhysicsInfo), with the same fields (MODDING_PLAN of each game).
struct Layout {
    int generation = 6;           // id Tech 5, 6, 7 or 8
    bool tech7 = false;           // id Tech 7 or later: no idGameLocal, the gate reads the player's physics game time
    bool tech5 = false;           // id Tech 5: fixed offsets, the gate reads the player's input command (game_tick)
    bool physics_ptr = false;     // id Tech 8: the player holds a pointer to its physics object
    bool mg6 = false;             // MachineGames' id Tech 6 (Wolfenstein II): fixed offsets, the view in the player
    bool mg7 = false;             // MachineGames' id Tech 7 (Indiana Jones): no player objects, only the input generator
    int64_t player_list = -1;     // idGameLocal.playerList (idEntityList<idPlayer>, 32 bytes; id Tech 6 only)
    int64_t physics = -1;         // idPlayer.physicsObj / physicsObjHavok (embedded) / idPlayerPhysicsInfo (pointer)
    int64_t view_angles = -1;     // <physics>.viewAngles (idAngles)
    int64_t physics_delta = -1;   // <physics>.deltaViewAngles (idAngles; id Tech 7 only)
    int64_t current = -1;         // <physics>.current (playerPState_t)
    int64_t delta_angles = -1;    // playerPState_t.deltaViewAngles (idAngles)
    int64_t game_time = -1;       // <physics>.currentGameTime (int64; id Tech 7 only: the gate's pause/load signal)
    int64_t cmd = -1;             // <physics>'s current input command (id Tech 5: 0x4c bytes, new every game tick)
    int64_t cmd_angles = -1;      // <physics>.current's input angles (id Tech 5: short[3], SHORT2ANGLE)
    bool complete() const;
};

// The fields behind the gyro's gate (IdTech::gate: pause, menus, inhibited input, the death camera), plus other
// candidates kept for diagnostics: all logged whenever one changes. Offsets resolved by name on the scan thread.
enum class Base { GameLocal, GameLocal2, Player };
struct Watch {
    const char* label;
    Base base;
    const char* cls;
    const char* field;
    int64_t extra;  // added to the field's offset (a member of an embedded struct)
    uint32_t bytes;  // 1, 4 or 8
    int64_t offset = -1;
};
Watch g_watches[] = {
    {"menu", Base::GameLocal, "idGameLocal", "frameParms", 0x14, 1},          // gameFrameParms_t.isMenuActive
    {"dialog", Base::GameLocal, "idGameLocal", "frameParms", 0x12, 1},        // .isDialogActive
    {"dialogPause", Base::GameLocal, "idGameLocal", "frameParms", 0x13, 1},   // .isDialogPausing
    {"advance", Base::GameLocal, "idGameLocal", "frameParms", 0x18, 1},       // .advanceTime
    {"menu2", Base::GameLocal2, "idGameLocal", "frameParms", 0x14, 1},        // the same, other gameLocal candidate
    {"advance2", Base::GameLocal2, "idGameLocal", "frameParms", 0x18, 1},
    {"pausedTime", Base::GameLocal, "idGameLocal", "gamePausedTime", 0, 4},
    {"gamestate", Base::GameLocal, "idGameLocal", "gamestate", 0, 4},
    {"skippable", Base::GameLocal, "idGameLocal", "skippableSync", 0, 8},
    {"inhibit", Base::Player, "idPlayer", "inhibitFlags", 0, 4},
    {"consumeGui", Base::Player, "idPlayer", "shouldConsumeNonGuiInput", 0, 1},
    {"interaction", Base::Player, "idPlayer", "inInteractionView", 0, 1},
    {"deathCam", Base::Player, "idPlayer", "deathCamera", 0, 8},
    {"arGuis", Base::Player, "idPlayer", "activeARGuis", 8, 4},              // idList num
    {"interactCam", Base::Player, "idPlayer", "dynamicInteractionCamera", 0, 8},
    {"slowCam", Base::Player, "idPlayer", "slowMotionCamera", 0, 8},
    {"specCam", Base::Player, "idPlayer", "spectatorCamera", 0, 8},
};

// Written by the scan thread before g_ready is set, read-only afterwards.
Image g_image;
TypeTable g_types;
Layout g_layout;
uintptr_t g_game_vtable = 0, g_player_vtable = 0, g_physics_vtable = 0;
uintptr_t g_cmdgen_vtable = 0;  // MachineGames id Tech 7: idUsercmdGenLocal's vtable (the stand-in player's check)
// id Tech 5 and MachineGames: exe offset of the input generator's angles (float pitch, yaw, roll), -1 = unknown. Found
// by the scan thread (id Tech 5: again whenever the game thread's check rejects it), read by the game thread.
std::atomic<int64_t> g_input_angles{-1};
std::atomic<bool> g_need_input{false};  // id Tech 5: the game thread's check rejected g_input_angles: search .data
// MachineGames id Tech 6: where idHavokPhysics_Player is embedded in idPlayer (scan thread only; the Game Pass 1.5.0.0
// offset first, any other build's found in the player).
int64_t g_mg6_physics = 0xeb68;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_scan_started{false};
bool g_tech5_files = false;  // detect(): the install has id Tech 5's resource files (set before the scan thread starts)
// Found by the scan thread, any time after g_ready.
std::atomic<uintptr_t> g_game_local{0};
std::atomic<uintptr_t> g_game_local2{0};  // the second heap candidate, if any (gate candidates only)
std::atomic<uintptr_t> g_heap_player{0};  // an idPlayer found in the heap by its vtable
std::atomic<bool> g_need_player{true};    // the game thread has no valid player: scan the heap for one

bool Layout::complete() const {
    if (mg6 || mg7) return g_input_angles >= 0;
    return physics >= 0 && view_angles >= 0 && current >= 0 && delta_angles >= 0;
}

// The input generator's angles in memory, 0 while unknown.
uintptr_t input_at() {
    const int64_t off = g_input_angles;
    return off < 0 ? 0 : g_image.base + uintptr_t(off);
}

// The one static object in .data starting with vtable: 0 if none or more than one. MachineGames' input generator
// (idUsercmdGenLocal) is found this way in every build and store, instead of at an exe offset.
uintptr_t single_static(uintptr_t vtable) {
    if (!vtable) return 0;
    const uintptr_t at = find_qword(g_image.data.begin, g_image.data.end, vtable);
    return at && !find_qword(at + 8, g_image.data.end, vtable) ? at : 0;
}

int64_t field(const char* cls, const char* name) {
    uint32_t size = 0;
    const int64_t off = g_types.field(cls, name, &size);
    if (off < 0) log("idtech: type info: %s.%s not found", cls, name);
    else log("idtech: type info: %s.%s at 0x%llx (%u bytes)", cls, name, (unsigned long long)off, size);
    return off;
}

// gameLocal: a static object in .data starting with its vtable, else a heap object.
// Whether gameLocal candidate g lists player in its playerList (idEntityList's layout isn't in the type info: each of its
// 4 qwords is tried as the player and as an array of player pointers).
bool lists_player(uintptr_t g, uintptr_t player) {
    uint64_t q[4] = {};
    if (!player || g_layout.player_list < 0 || !mem::guarded_copy(q, g + uintptr_t(g_layout.player_list), sizeof q))
        return false;
    for (uint64_t v : q) {
        if (v == player) return true;
        if (mem::plausible(v))
            for (int k = 0; k < 4; ++k)
                if (mem::rdp(v + 8 * k) == player) return true;
    }
    return false;
}

// gameLocal: every object with idGameLocal's vtable (.data, then the heap); the one whose playerList holds the local
// player is confirmed, else the first is used provisionally. Called every 5 s until confirmed.
std::atomic<uintptr_t> g_current_player{0};  // the game thread's player, for the check
std::atomic<bool> g_game_confirmed{false};
HANDLE g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // wakes the scan thread (player or gameLocal needed)
void find_game_local() {
    if (!g_game_vtable) return;
    uintptr_t hits[8];
    size_t n = 0;
    if (const uintptr_t s = find_qword(g_image.data.begin, g_image.data.end, g_game_vtable)) hits[n++] = s;
    uint64_t bytes = 0;
    const ULONGLONG start = GetTickCount64();
    n += heap_find(g_game_vtable, hits + n, 8 - n, &bytes);
    const uintptr_t player = g_current_player;
    uintptr_t pick = 0;
    for (size_t i = 0; i < n && !pick; ++i)
        if (lists_player(hits[i], player)) pick = hits[i];
    if (pick) g_game_confirmed = true;
    else if (n) pick = hits[0];
    if (!pick || (pick == g_game_local && !g_game_confirmed)) return;
    std::string all;
    for (size_t i = 0; i < n; ++i) all += " " + mem::module_offset(hits[i]);
    log("idtech: gameLocal %s (%s; candidates:%s; %.2f GB scanned, %llu ms)", mem::module_offset(pick).c_str(),
        g_game_confirmed ? "its playerList holds the player" : "provisional: no player to check yet", all.c_str(),
        double(bytes) / (1 << 30), GetTickCount64() - start);
    g_game_local = pick;
    g_game_local2 = n > 1 ? (hits[0] == pick ? hits[1] : hits[0]) : 0;
}

// The player's physics object: embedded (id Tech 6/7) or behind a pointer (id Tech 8). 0 if unreadable.
uintptr_t physics_of(uintptr_t player) {
    if (!player || g_layout.physics < 0) return 0;
    const uintptr_t at = player + uintptr_t(g_layout.physics);
    return g_layout.physics_ptr ? mem::rdp(at) : at;
}

// The player straight from the heap: an object with idPlayer's vtable whose physics object (embedded or by pointer)
// starts with its physics class's vtable.
// MachineGames id Tech 6: the physics object embedded in player, at g_mg6_physics or, the first time it isn't there
// (another build), wherever the player's first 0x20000 bytes hold its vtable (8-aligned).
bool mg6_physics(uintptr_t player) {
    if (mem::rdp(player + uintptr_t(g_mg6_physics)) == g_physics_vtable) return true;
    static bool searched = false;
    if (searched) return false;
    const uintptr_t at = find_qword(player + 8, player + 0x20000, g_physics_vtable);
    if (!at) return false;
    searched = true;
    log("idtech: MachineGames id Tech 6: idHavokPhysics_Player at idPlayer+0x%llx (not +0x%llx)",
        (unsigned long long)(at - player), (unsigned long long)g_mg6_physics);
    g_mg6_physics = int64_t(at - player);
    return true;
}

void find_heap_player() {
    if (!g_player_vtable || !g_physics_vtable || (g_layout.physics < 0 && !g_layout.mg6)) return;
    uintptr_t hits[64];
    uint64_t bytes = 0;
    const ULONGLONG start = GetTickCount64();
    const size_t n = heap_find(g_player_vtable, hits, 64, &bytes);
    size_t valid = 0;
    uintptr_t player = 0;
    for (size_t i = 0; i < n; ++i)
        if (g_layout.mg6 ? mg6_physics(hits[i]) : mem::rdp(physics_of(hits[i])) == g_physics_vtable) {
            if (!player) player = hits[i];
            ++valid;
        }
    static size_t last_valid = size_t(-1);
    if (valid != last_valid) {
        last_valid = valid;
        log("idtech: heap scan: %zu idPlayer vtables, %zu with a physicsObj (%.2f GB, %llu ms)", n, valid,
            double(bytes) / (1 << 30), GetTickCount64() - start);
    }
    if (player) g_heap_player = player;
}

// id Tech 5: the input generator's angles when the game thread's check rejected the Game Pass 1.14.1.0 address (another
// build or store): the one float triple in .data equal to the player's input command angles (the generator turns its
// floats into the command's shorts every tick; yaw unwrapped, so compared modulo 360). Zeros are everywhere: only tried
// with a command yaw away from 0, and kept only when a single triple matches.
void find_tech5_input() {
    const uintptr_t physics = physics_of(g_current_player);
    int16_t cmd[3] = {};
    if (!physics || !mem::guarded_copy(cmd, physics + uintptr_t(g_layout.current) + uintptr_t(g_layout.cmd_angles),
                                       sizeof cmd))
        return;
    constexpr float kShort = 360.0f / 65536.0f;
    const float want[3] = {cmd[0] * kShort, cmd[1] * kShort, cmd[2] * kShort};
    if (std::fabs(want[1]) < 1.0f) return;
    uintptr_t found = 0;
    size_t matches = 0;
    float page[0x400 + 2];
    for (uintptr_t p = g_image.data.begin & ~uintptr_t(0xFFF); p < g_image.data.end; p += 0x1000) {
        // Each page plus the next 8 bytes (a triple across the boundary), else the page alone.
        const bool more = mem::guarded_copy(page, p, sizeof page);
        if (!more && !mem::guarded_copy(page, p, 0x1000)) continue;
        const size_t starts = (more ? 0x400 + 2 : 0x400) - 2;
        for (size_t i = 0; i < starts; ++i) {
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k)
                ok = std::isfinite(page[i + k]) && std::fabs(std::remainder(page[i + k] - want[k], 360.0f)) < 0.01f;
            if (ok && ++matches == 1) found = p + 4 * i;
        }
    }
    static size_t last = size_t(-1);
    if (matches != last) {
        last = matches;
        log("idtech: id Tech 5 input angles search (command pitch %.2f, yaw %.2f): %zu matches in .data%s", want[0],
            want[1], matches, matches == 1 ? (" at " + mem::module_offset(found)).c_str() : "");
    }
    if (matches != 1) return;
    g_input_angles = int64_t(found - g_image.base);
    g_need_input = false;
}

DWORD WINAPI scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    if (!g_image.load()) {
        log("idtech: the exe's sections can't be read");
        return 0;
    }
    const size_t classes = g_types.load(g_image);
    log("idtech: type info: %zu classes (%llu ms)", classes, GetTickCount64() - start);
    g_layout.generation = g_tech5_files                            ? 5
                          : g_types.find("idPhysics_Player")        ? 6
                          : g_types.find("idHavokPhysics_Player") ? 7
                          : g_types.find("idPlayerPhysicsInfo")   ? 8
                                                                  : 6;
    g_layout.tech7 = g_layout.generation >= 7;
    g_layout.tech5 = g_layout.generation == 5;
    g_layout.physics_ptr = g_layout.generation == 8 || g_layout.tech5;
    // MachineGames' id Tech 6 (Wolfenstein II): neither idPlayer nor a player physics class in the type info (only
    // nested structs such as idHavokPhysics_Player::crusher_t), but idHavokPhysics_Player in the RTTI.
    g_layout.mg6 = g_layout.generation == 6 && !g_types.find("idPlayer") && !g_types.find("idPhysics_Player") &&
                   rtti_vtable(g_image, ".?AVidHavokPhysics_Player@@");
    // MachineGames' id Tech 7 (Indiana Jones): not even an idPlayer in the RTTI (an entity-component game), but the
    // input generator idUsercmdGenLocal.
    g_layout.mg7 = g_layout.generation == 6 && !g_layout.mg6 && !g_types.find("idPlayer") &&
                   !rtti_vtable(g_image, ".?AVidPlayer@@") && rtti_vtable(g_image, ".?AVidUsercmdGenLocal@@");
    const char* physics_class = g_layout.generation == 8               ? "idPlayerPhysicsInfo"
                                : g_layout.generation == 7 || g_layout.mg6 ? "idHavokPhysics_Player"
                                                                           : "idPhysics_Player";
    const char* physics_field = g_layout.generation == 8   ? "idPlayerPhysicsInfo"
                                : g_layout.generation == 7 ? "physicsObjHavok"
                                                           : "physicsObj";
    if (g_layout.mg7) log("idtech: MachineGames id Tech 7 (%s type info; no player objects)", g_types.layout());
    else
        log("idtech: id Tech %d (%s type info; player physics %s%s)", g_layout.generation, g_types.layout(),
            physics_class, g_layout.physics_ptr ? ", by pointer" : "");
    if (g_layout.tech5) {
        // id Tech 5 (MODLOG 2026-10-19, Game Pass 1.14.1.0): idPlayer+0x310 points to the idPhysics_Player; there,
        // SetPlayerInput (exe+0x411930) stores the input command at +0x43c and the view angles at +0x488, copied from
        // the player state at +0x343c, whose SetViewAngles (exe+0x60D130) keeps deltaViewAngles (+0x13c) = view -
        // SHORT2ANGLE(the command's angles, short[3] at +0x60): view = command angles + deltaViewAngles. The command's
        // angles come from the input generator's float angles in the exe (+0x1c1cb40), where the stick and mouse turns
        // add up. The gyro adds there too: on foot the view follows 1:1, and a mounted gun (the plane turret) turns
        // with its own ratio and limits, as with the stick; the gun ignores deltaViewAngles. The generator is
        // idUsercmdGenLocal, the one static object in .data with its vtable (exe+0x1c1cb30 in Game Pass 1.14.1.0), its
        // angles at +0x10 as in Wolfenstein II: found that way in any build or store. Else the Game Pass address, which
        // check_tech5 rejects in any other build; the scan thread then searches them by their values (find_tech5_input).
        g_cmdgen_vtable = rtti_vtable(g_image, ".?AVidUsercmdGenLocal@@");
        const uintptr_t gen = single_static(g_cmdgen_vtable);
        g_input_angles = gen ? int64_t(gen - g_image.base) + 0x10 : 0x1c1cb40;
        log("idtech: id Tech 5 idUsercmdGenLocal %s: input angles at %s", gen ? mem::module_offset(gen).c_str() : "not found",
            mem::module_offset(g_image.base + uintptr_t(int64_t(g_input_angles))).c_str());
        g_layout.physics = 0x310;
        g_layout.view_angles = 0x488;
        g_layout.cmd = 0x43c;
        g_layout.current = 0x343c;
        g_layout.delta_angles = 0x13c;
        g_layout.cmd_angles = 0x60;
        log("idtech: id Tech 5 offsets (no player fields in its type info): physics at idPlayer+0x310, viewAngles +0x488, "
            "deltaViewAngles +0x3578, input command +0x43c (checked against the game's own equation)");
    } else if (g_layout.mg6) {
        // Wolfenstein II (MODLOG 2026-10-20, Game Pass 1.5.0.0): one idPlayer in the heap with its
        // idHavokPhysics_Player embedded at +0xeb68 (mg6_physics: another build's offset is found in the player), whose
        // view angles (physics+0xff8) are the input generator's angles plus a constant delta (yaw -175.28, pitch +80.05
        // over a 15 s stick recording). The input generator (idUsercmdGenLocal) is a static object (exe+0x3db7980 in
        // that build), found by its vtable as in Indiana Jones; its accumulated float angles (pitch, yaw, roll; yaw
        // unwrapped) at +0x10: the gyro adds there, as in id Tech 5.
        g_cmdgen_vtable = rtti_vtable(g_image, ".?AVidUsercmdGenLocal@@");
        if (const uintptr_t gen = single_static(g_cmdgen_vtable)) {
            g_input_angles = int64_t(gen - g_image.base) + 0x10;
            log("idtech: MachineGames id Tech 6 (no player fields in its type info): idUsercmdGenLocal at %s, input "
                "angles +0x10; physics (idHavokPhysics_Player) at idPlayer+0xeb68 or found in it",
                mem::module_offset(gen).c_str());
        } else {
            log("idtech: MachineGames id Tech 6: no single idUsercmdGenLocal in .data (vtable %s): no scene",
                mem::module_offset(g_cmdgen_vtable).c_str());
        }
    } else if (g_layout.mg7) {
        // Indiana Jones (MODLOG 2026-10-20, Game Pass 1.16.1.0): one idUsercmdGenLocal, static in .data
        // (exe+0x64a9a20 in that build), found by its vtable; its accumulated float angles (pitch, yaw; yaw unwrapped)
        // at +0x9a8 drive the view (a live +30 yaw / -15 pitch write turned it and held). +0x9b4/+0x9b8 are the
        // stick's per-frame rates.
        g_cmdgen_vtable = rtti_vtable(g_image, ".?AVidUsercmdGenLocal@@");
        const uintptr_t gen = single_static(g_cmdgen_vtable);
        float angles[2] = {};
        if (gen && mem::guarded_copy(angles, gen + 0x9a8, sizeof angles) && std::isfinite(angles[0]) &&
            std::isfinite(angles[1])) {
            g_input_angles = int64_t(gen - g_image.base) + 0x9a8;
            log("idtech: MachineGames id Tech 7: idUsercmdGenLocal at %s, input angles +0x9a8 (pitch %.2f, yaw %.2f)",
                mem::module_offset(gen).c_str(), angles[0], angles[1]);
        } else {
            log("idtech: MachineGames id Tech 7: no single idUsercmdGenLocal in .data (vtable %s): no scene",
                mem::module_offset(g_cmdgen_vtable).c_str());
        }
    } else {
        if (!g_layout.tech7) g_layout.player_list = field("idGameLocal", "playerList");
        g_layout.physics = field("idPlayer", physics_field);
        g_layout.view_angles = field(physics_class, "viewAngles");
        if (g_layout.tech7) g_layout.physics_delta = field(physics_class, "deltaViewAngles");
        g_layout.current = field(physics_class, "current");
        g_layout.delta_angles = field("playerPState_t", "deltaViewAngles");
        if (g_layout.tech7) g_layout.game_time = field(physics_class, "currentGameTime");
    }
    for (Watch& w : g_watches) {
        if (g_layout.tech5 || g_layout.mg6 || g_layout.mg7) break;  // none of the gate fields are in their type info
        const int64_t off = g_types.field(w.cls, w.field);
        w.offset = off < 0 ? -1 : off + w.extra;
        if (off < 0) log("idtech: type info: %s.%s not found (gate candidate %s)", w.cls, w.field, w.label);
    }

    // id Tech 5: no playerList to tell its 4 idGameLocal instances apart, and the gate doesn't need it.
    g_game_vtable = g_layout.tech5 || g_layout.mg6 || g_layout.mg7 ? 0 : rtti_vtable(g_image, ".?AVidGameLocal@@");
    g_player_vtable = rtti_vtable(g_image, ".?AVidPlayer@@");
    g_physics_vtable = rtti_vtable(g_image, (std::string(".?AV") + physics_class + "@@").c_str());
    log("idtech: RTTI vtables: idGameLocal %s, idPlayer %s, %s %s (%llu ms)", mem::module_offset(g_game_vtable).c_str(),
        mem::module_offset(g_player_vtable).c_str(), physics_class, mem::module_offset(g_physics_vtable).c_str(),
        GetTickCount64() - start);
    // No player layout (an id Tech generation not mapped yet, e.g. id Tech 5): no scene, and no heap scans for it.
    if (!g_layout.complete()) {
        log("idtech: the player's view fields aren't all in the type info: no scene (the gyro has no view to turn)");
        return 0;
    }
    g_ready = true;
    // WriteWatch=player+<hex offset>: the local idPlayer (finding the source of a view without type info).
    writewatch::provide("player", [] { return g_current_player.load(); });
    // Then every 5 s: gameLocal until found, and a player whenever the game thread has none (loading, front end).
    // Every 5 s, every second while the player or gameLocal is missing, and at once when the game thread asks.
    for (bool logged = false;; WaitForSingleObject(g_wake, g_need_player || !g_game_confirmed ? 1000 : 5000)) {
        // gameLocal is recreated with the level: a confirmed one that no longer lists the current player is searched
        // for again.
        if (g_game_confirmed && g_current_player && !lists_player(g_game_local, g_current_player)) {
            log("idtech: gameLocal %s no longer lists the player: searching again", mem::module_offset(g_game_local).c_str());
            g_game_confirmed = false;
        }
        if (!g_game_confirmed && g_game_vtable) {
            find_game_local();
            if (!g_game_local && !logged) {
                logged = true;
                log("idtech: gameLocal not found yet (.data or heap; retrying every 5 s)");
            }
        }
        if (g_need_player) find_heap_player();
        if (g_layout.tech5 && g_need_input) find_tech5_input();
    }
}

bool is_player(uintptr_t p) {
    if (!mem::plausible(p)) return false;
    if (g_player_vtable && mem::rdp(p) == g_player_vtable) return true;
    return rtti_name(g_image, p).find("Player@@") != std::string::npos;
}

struct Angles {
    float pitch = 0, yaw = 0, roll = 0;
};

class IdTech final : public Engine {
public:
    const char* name() const override { return "idTech"; }

    bool detect(const std::wstring& exe_path) const override {
        const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
        // DOOM 2016 and Eternal: base\gameresources.resources; The Dark Ages: base\common.resources; Indiana Jones and
        // the Great Circle (MachineGames' id Tech 7): base\gameresources_pc.resources.
        auto has = [&](const wchar_t* file) { return GetFileAttributesW((dir + file).c_str()) != INVALID_FILE_ATTRIBUTES; };
        for (const wchar_t* marker :
             {L"\\base\\gameresources.resources", L"\\base\\common.resources", L"\\base\\gameresources_pc.resources"})
            if (has(marker)) return true;
        // id Tech 5 (Wolfenstein: The New Order): base\master.index over base\chunk<N>.resources.
        g_tech5_files = has(L"\\base\\master.index") && has(L"\\base\\chunk0.resources");
        return g_tech5_files;
    }

    void configure() override {}
    bool adopt_pump_thread() const override { return true; }

    // id's mod launcher next to the game (DOOM Eternal, The Dark Ages): it pumps messages from the start, so it would
    // run Gyro4All and keep the session from the game it starts.
    bool is_launcher(const std::wstring& exe_path) const override {
        const std::wstring file = exe_path.substr(exe_path.find_last_of(L"\\/") + 1);
        return _wcsicmp(file.c_str(), L"idTechLauncher.exe") == 0;
    }

    std::vector<ViewInfo> views() const override { return {{1, "default", 0}}; }

    EngineFrame read_frame() override {
        if (!g_scan_started.exchange(true)) {
            if (HANDLE t = CreateThread(nullptr, 0, &scan_thread, nullptr, 0, nullptr)) CloseHandle(t);
        }
        if (g_ready) {
            find_player();
            g_current_player = player_;
            log_watches();
        }
        return gate();
    }

    // GyroLib's turn (degrees; pitch up, yaw right) added to the physics object's current.deltaViewAngles (pitch down,
    // yaw left): the delta every generation keeps (id Tech 7/8 also wrap its yaw by 360 degrees themselves; the physics
    // object's own deltaViewAngles stays 0, MODLOG 2026-10-19). id Tech 8 recomputes this delta every tick during some
    // animations (weapon reloads, spawning) to make the camera follow them: the gyro's writes are lost meanwhile (the
    // mouse, through the input command, still works there).
    bool apply(double pitch, double yaw) override {
        if (!player_) return false;
        const uintptr_t physics = g_layout.mg6 || g_layout.mg7 ? player_ : physics_of(player_);
        if (!physics) return false;
        if (g_layout.tech5 && tech5_checked_ != player_) {
            const ULONGLONG now = GetTickCount64();
            if (now < tech5_next_check_) return false;
            tech5_next_check_ = now + 1000;
            if (!check_tech5()) return false;
            tech5_checked_ = player_;
        }
        if (g_layout.tech5 || g_layout.mg6 || g_layout.mg7) {
            // The input generator's angles (pitch down, yaw left), like a stick turn: no pitch limit here, the game
            // clamps the view itself (and a gun to its own limits).
            const uintptr_t at = input_at();
            float input[2];
            if (!at || !mem::guarded_copy(input, at, sizeof input) || !std::isfinite(input[0]) || !std::isfinite(input[1]))
                return false;
            input[0] -= float(pitch);
            input[1] -= float(yaw);
            if (!mem::guarded_store(at, input, sizeof input)) return false;
            if (!first_write_logged_) {
                first_write_logged_ = true;
                log("idtech: first gyro write to the input angles at %s", mem::module_offset(at).c_str());
            }
            return true;
        }
        const uintptr_t delta_at = physics + uintptr_t(g_layout.current) + uintptr_t(g_layout.delta_angles);
        Angles view, delta;
        if (!mem::rd(physics + uintptr_t(g_layout.view_angles), view) || !mem::rd(delta_at, delta)) return false;
        if (!std::isfinite(view.pitch) || !std::isfinite(delta.pitch) || !std::isfinite(delta.yaw)) return false;
        // Pitch: only as far as the limits allow from the current view.
        const float from = std::clamp(view.pitch, kMinPitch, kMaxPitch);
        delta.pitch += std::clamp(from - float(pitch), kMinPitch, kMaxPitch) - from;
        delta.yaw = std::remainder(delta.yaw - float(yaw), 360.0f);
        if (!mem::guarded_store(delta_at, &delta, sizeof(float) * 2)) return false;
        if (!first_write_logged_) {
            first_write_logged_ = true;
            log("idtech: first gyro write to deltaViewAngles at %s", mem::module_offset(delta_at).c_str());
        }
        return true;
    }

    // id Tech 6+ (deltaViewAngles path): the view stops short of kMin/kMaxPitch (DOOM 2016: pushing the gyro to the top or
    // bottom made the core's stick fallback take the stopped view for writes the game undid, MODLOG 2026-10-31). The
    // limit isn't read from the game: a view beyond 45 degrees that stays put for kStuckMs (several 60 Hz ticks: the view
    // only moves on the game's ticks) while every turn asks for more is taken as the limit there, and the turn stops at
    // it from then on (raised again if the view ever goes past it).
    void limit_turn(double& pitch, double& /*yaw*/) override {
        if (g_layout.tech5 || g_layout.mg6 || g_layout.mg7 || !player_) return;
        const uintptr_t physics = physics_of(player_);
        Angles view;
        if (!physics || !mem::rd(physics + uintptr_t(g_layout.view_angles), view) || !std::isfinite(view.pitch)) return;
        const double up = -double(view.pitch);  // gyro sense
        const ULONGLONG now = precise_ms();
        const bool recent = now - asked_at_ < 100;
        const bool still = std::abs(up - last_up_) < 0.05;
        const int pushing = !recent || !still ? 0 : asked_up_ > up + 0.25 && up > 45 ? 1 : asked_up_ < up - 0.25 && up < -45 ? -1 : 0;
        if (!pushing || pushing != stuck_dir_) {
            stuck_dir_ = pushing;
            stuck_since_ = now;
        } else if (now - stuck_since_ >= kStuckMs) {
            if (pushing > 0 && up < top_) {
                top_ = up;
                log("idtech: the view stops at %.1f degrees up: the gyro's pitch stops there", top_);
            } else if (pushing < 0 && up > bottom_) {
                bottom_ = up;
                log("idtech: the view stops at %.1f degrees down: the gyro's pitch stops there", -bottom_);
            }
        }
        if (up > top_) top_ = std::min(up, double(kMaxPitch));
        if (up < bottom_) bottom_ = std::max(up, double(kMinPitch));
        pitch = std::clamp(pitch, bottom_ - up, top_ - up);
        asked_up_ = up + pitch;
        asked_at_ = now;
        last_up_ = up;
    }

    // viewAngles in GyroLib's senses (pitch up, yaw right), for the core's checks; camera = the player. id Tech 5: the
    // input angles the gyro writes (a gun follows them at its own ratio, the view may stop at its pitch limits: neither
    // is a write the game undid).
    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        Angles view;
        if (g_layout.tech5 || g_layout.mg6 || g_layout.mg7) {
            if (!player_ || !input_at() || !mem::guarded_copy(&view, input_at(), sizeof(float) * 2))
                return false;
        } else if (!player_ || !physics_of(player_) ||
                   !mem::rd(physics_of(player_) + uintptr_t(g_layout.view_angles), view)) {
            return false;
        }
        if ((g_layout.tech7 || g_layout.tech5) && precise_ms() - game_time_changed_ > kTickMs) return false;
        pitch = -view.pitch;
        yaw = -view.yaw;
        camera = player_;
        return true;
    }

    std::string status_scene() const override {
        return std::format("view={} gameLocal={} player={}", player_ ? "default" : "none",
                           g_game_local ? mem::module_offset(g_game_local) : "-",
                           player_ ? mem::module_offset(player_) : "-");
    }

private:
    // The local player: the first idPlayer reachable from gameLocal.playerList. The list's layout isn't in the type
    // info (a template), so each of its 4 qwords is tried as the player itself and as an array of player pointers.
    void find_player() {
        if (g_layout.mg7) {
            // No player objects: the input generator stands in (static, so it stays; checked by its vtable).
            const uintptr_t gen = input_at() - 0x9a8;
            const uintptr_t now = mem::rdp(gen) == g_cmdgen_vtable ? gen : 0;
            if (now != player_) {
                player_ = now;
                g_need_player = false;
                log("idtech: input generator %s %s", mem::module_offset(gen).c_str(), now ? "found" : "gone");
            }
            return;
        }
        if (player_ && is_player(player_)) return;
        if (player_) {
            log("idtech: player %s gone", mem::module_offset(player_).c_str());
            player_ = 0;
        }
        if (!g_need_player.exchange(true)) SetEvent(g_wake);
        const ULONGLONG now = GetTickCount64();
        if (!g_layout.complete() || now < next_player_try_) return;
        next_player_try_ = now + 1000;
        if (const uintptr_t heap = g_heap_player.exchange(0); heap && is_player(heap)) {
            player_ = heap;
            log("idtech: player %s (%s, heap scan)", mem::module_offset(player_).c_str(),
                rtti_name(g_image, player_).c_str());
            g_need_player = false;
            return;
        }
        if (!g_game_local) return;
        const uintptr_t list = g_game_local + uintptr_t(g_layout.player_list);
        uint64_t q[4] = {};
        if (!mem::guarded_copy(q, list, sizeof q)) return;
        for (int i = 0; i < 4 && !player_; ++i) {
            if (is_player(q[i])) {
                player_ = q[i];
                how_ = std::format("playerList qword {}", i);
            } else if (mem::plausible(q[i])) {
                for (int k = 0; k < 4 && !player_; ++k)
                    if (const uintptr_t p = mem::rdp(q[i] + 8 * k); is_player(p)) {
                        player_ = p;
                        how_ = std::format("playerList qword {} [{}]", i, k);
                    }
            }
        }
        if (player_) {
            g_need_player = false;
            log("idtech: player %s (%s, %s), playerList %016llx %016llx %016llx %016llx", mem::module_offset(player_).c_str(),
                rtti_name(g_image, player_).c_str(), how_.c_str(), q[0], q[1], q[2], q[3]);
        } else if (!list_logged_) {
            list_logged_ = true;
            log("idtech: no player in playerList yet: %016llx %016llx %016llx %016llx", q[0], q[1], q[2], q[3]);
        }
    }

    // A value that changes every game tick while the game runs: id Tech 7/8 the physics game time, id Tech 5 a hash of
    // the player's input command (it carries the tick's time and frame number). False = unreadable.
    bool game_tick(int64_t& t) const {
        const uintptr_t physics = physics_of(player_);
        if (!physics) return false;
        if (g_layout.tech5) {
            uint8_t cmd[0x4c];
            if (!mem::guarded_copy(cmd, physics + uintptr_t(g_layout.cmd), sizeof cmd)) return false;
            uint64_t h = 1469598103934665603ull;  // FNV-1a
            for (uint8_t b : cmd) h = (h ^ b) * 1099511628211ull;
            t = int64_t(h);
            return true;
        }
        return g_layout.game_time >= 0 && mem::rd(physics + uintptr_t(g_layout.game_time), t);
    }

    // id Tech 5's fixed offsets, checked on each new player: the command's yaw is the input angles' yaw as a short (the
    // input generator's), and view yaw = SHORT2ANGLE(the command's yaw) + deltaViewAngles yaw. A mismatch (another
    // build) leaves the gyro off; when only the input angles' address is wrong (it is the Game Pass build's), the scan
    // thread searches .data for them (find_tech5_input) and the next check tries its result.
    bool check_tech5() const {
        const uintptr_t physics = physics_of(player_);
        const uintptr_t state = physics + uintptr_t(g_layout.current);
        Angles view, delta;
        int16_t angles[3] = {};
        if (!physics || !mem::rd(physics + uintptr_t(g_layout.view_angles), view) ||
            !mem::rd(state + uintptr_t(g_layout.delta_angles), delta) ||
            !mem::guarded_copy(angles, state + uintptr_t(g_layout.cmd_angles), sizeof angles))
            return false;
        constexpr float kShort = 360.0f / 65536.0f;
        const float command = angles[1] * kShort;
        const bool view_ok = std::isfinite(view.yaw) && std::fabs(std::remainder(command + delta.yaw - view.yaw, 360.0f)) < 0.05f;
        // A tick may pass between the reads (the stick or mouse turning): a few degrees of slack. A yaw of 0 on both
        // sides proves nothing (any zeroed memory): checked again once the player has turned.
        float input[2] = {};
        const uintptr_t at = input_at();
        const bool input_ok = at && mem::guarded_copy(input, at, sizeof input) && std::isfinite(input[1]) &&
                              std::fabs(std::remainder(input[1] - command, 360.0f)) < 3.0f &&
                              (std::fabs(command) >= 1.0f || input[1] != 0.0f);
        if (view_ok && !input_ok && !g_need_input.exchange(true)) SetEvent(g_wake);
        const bool ok = view_ok && input_ok;
        const int state_now = ok ? 1 : view_ok ? 2 : 0;
        if (state_now == last_check_) return ok;  // logged on each change only (retried every second while it fails)
        last_check_ = state_now;
        log("idtech: id Tech 5 check, view yaw %.3f = command %.3f + delta %.3f, input angles yaw %.3f at %s: %s",
            view.yaw, command, delta.yaw, input[1], at ? mem::module_offset(at).c_str() : "-",
            ok        ? "ok"
            : view_ok ? "input angles not there (another build or store?): searching .data for them"
                      : "MISMATCH (another build? the gyro stays off)");
        return ok;
    }

    // A watched field's value by label; false = unknown (no object yet, field not in the type info, unreadable).
    bool watch(const char* label, uint64_t& v) const {
        for (const Watch& w : g_watches) {
            if (std::strcmp(w.label, label)) continue;
            const uintptr_t base = w.base == Base::Player      ? player_
                                   : w.base == Base::GameLocal ? g_game_local.load()
                                                               : g_game_local2.load();
            v = 0;
            return base && w.offset >= 0 && mem::guarded_copy(&v, base + uintptr_t(w.offset), w.bytes);
        }
        return false;
    }

    // The gyro's gate, from what the game itself does (MODLOG 2026-10-19, matched to pause, map, glory kills in game).
    // gameLocal's flags are only read from the gameLocal that lists this player (it is recreated with each level or
    // checkpoint load; the old one reads 0s, and gamePausedTime 0 looked like a pause), checked every frame:
    // - no gameplay view: no such gameLocal yet, or the game's time not advancing (gameFrameParms_t.advanceTime 0:
    //   loading; the game undid the gyro's writes then), the player's input inhibited by the game
    //   (idUCmdTracker::inhibitFlags_t nonzero: glory kills, the map, scripted moments), or the death camera;
    // - paused: the engine's menu flag (gameFrameParms_t.isMenuActive: pause menu and its dialogs) or the game's pause
    //   time set (gamePausedTime is -1 while running: pause menu, map/codex);
    // - cursor: the player's GUI consumes the input (shouldConsumeNonGuiInput).
    EngineFrame gate() {
        EngineFrame f;
        f.in_game = player_ != 0;
        if (!player_) return f;
        const bool game = g_game_confirmed && lists_player(g_game_local, player_);
        if (!game && g_game_confirmed) {
            g_game_confirmed = false;  // a stale gameLocal: the scan thread looks for the new one right away
            SetEvent(g_wake);
        }
        uint64_t v = 0;
        const char* why = nullptr;
        if (g_layout.tech7 || g_layout.tech5) {
            // id Tech 7 (DOOM Eternal) has no idGameLocal: the player's own physics game time says whether the game
            // runs (id Tech 5: its input command, a new one every tick). Frozen for over kFrozenMs (it advances every
            // game tick, 60 Hz): paused (pause menu, map) or loading.
            // And a new player's first 2 s of advancing game time: the game sets its view up to ~1.2 s after its time
            // starts (it undid the gyro's writes then and started the stick fallback, MODLOG 2026-10-19).
            const ULONGLONG now = precise_ms();
            if (player_ != gated_player_) {
                gated_player_ = player_;
                game_time_changed_ = now;
                player_since_ = 0;  // set when its game time first advances
                last_game_time_ = 0;
            }
            int64_t t = 0;
            if (game_tick(t) && t != last_game_time_) {
                if (last_game_time_ && !player_since_) player_since_ = now;
                last_game_time_ = t;
                game_time_changed_ = now;
            }
            const bool frozen = now - game_time_changed_ > kFrozenMs;
            if (frozen != frozen_logged_) {
                frozen_logged_ = frozen;
                log("idtech: game time %s", frozen ? "frozen (paused or loading)" : "advancing");
            }
            if (frozen) f.paused = true;
            if (!player_since_ || now - player_since_ < 2000) why = "player just spawned";
        } else if (g_layout.mg6 || g_layout.mg7) {
            // No pause or menu signal mapped yet: the game pauses itself when it loses focus (the core's focus gate
            // covers that); its pause menu and cutscenes are still to find.
        } else if (!game) {
            why = "waiting for the game's state";
        } else {
            if (watch("menu", v) && v) f.paused = true;
            if (watch("pausedTime", v) && uint32_t(v) != 0xFFFFFFFFu) f.paused = true;
            if (watch("advance", v) && !v) why = "game time not advancing (loading)";
        }
        if (watch("consumeGui", v) && v) f.cursor = true;
        if (!why && watch("inhibit", v) && v) why = "player input inhibited";
        if (!why && watch("deathCam", v) && v) why = "death camera";
        f.view_id = why ? 0 : 1;
        f.target = why ? why : g_layout.mg7 ? "idUsercmdGenLocal" : "idPlayer";
        return f;
    }

    // The gate candidates (g_watches) and the Windows cursor, one line whenever any of them changes ("-" = unreadable).
    void log_watches() {
        std::string line;
        for (const Watch& w : g_watches) {
            const uintptr_t base = w.base == Base::Player      ? player_
                                   : w.base == Base::GameLocal ? g_game_local.load()
                                                               : g_game_local2.load();
            uint64_t v = 0;
            const bool ok = base && w.offset >= 0 && mem::guarded_copy(&v, base + uintptr_t(w.offset), w.bytes);
            line += std::format(" {}={}", w.label, ok ? std::format("{:x}", v) : "-");
        }
        CURSORINFO ci{sizeof ci};
        line += std::format(" cursor={}", GetCursorInfo(&ci) ? int(ci.flags & CURSOR_SHOWING) : -1);
        if (line == last_watch_line_) return;
        last_watch_line_ = line;
        // At most 20 lines a second (some candidates may flicker).
        const ULONGLONG now = GetTickCount64();
        if (now - watch_window_ >= 1000) {
            watch_window_ = now;
            watch_lines_ = 0;
        }
        if (++watch_lines_ <= 20) log("idtech: gate%s", line.c_str());
    }

    bool first_write_logged_ = false;
    // limit_turn: the view's pitch limits as seen (gyro sense, degrees) and the pitch the last turn asked for.
    double top_ = kMaxPitch, bottom_ = kMinPitch, asked_up_ = 0, last_up_ = 0;
    ULONGLONG asked_at_ = 0, stuck_since_ = 0;
    int stuck_dir_ = 0;
    uintptr_t player_ = 0;
    uintptr_t tech5_checked_ = 0;  // id Tech 5: the player whose offsets passed check_tech5
    ULONGLONG tech5_next_check_ = 0;
    mutable int last_check_ = -1;
    std::string how_;
    bool list_logged_ = false;
    ULONGLONG next_player_try_ = 0;
    std::string last_watch_line_;
    ULONGLONG watch_window_ = 0;
    int watch_lines_ = 0;
    uintptr_t gated_player_ = 0;
    ULONGLONG player_since_ = 0;
    int64_t last_game_time_ = 0;
    ULONGLONG game_time_changed_ = 0;
    bool frozen_logged_ = false;
};

IdTech g_engine;

}  // namespace

Engine& engine() { return g_engine; }

}  // namespace gyro4all::idtech
