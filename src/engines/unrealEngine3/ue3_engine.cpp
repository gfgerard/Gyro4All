// Unreal Engine 3: Mass Effect Legendary Edition (BioWare's UE3 fork, 2021) first. A UE3 install: <root>\Binaries\Win64\<exe>
// and the cooked packages in <root>\<Game>\CookedPC* (CookedPC, CookedPCConsole, CookedPCConsole_<lang>), e.g. Mass Effect
// LE's Game\ME1\BioGame\CookedPCConsole. UE4/UE5 builds have Content\Paks instead. The Mass Effect LE exes are EA .ooa
// stubs: the core sees the section and hooks inline.
//
// The view (MODLOG 2026-10-22, Mass Effect LE 1): the local player's controller's Actor.Rotation, an int32 rotator (65536
// units per turn; pitch positive up, stored 0..65535; yaw unwrapped), the UE3 counterpart of UE4's ControlRotation: the
// camera follows it (Dishonored too, MODLOG 2026-10-26). Everything is found at run time, nothing per build:
// - GObjects: a TArray<UObject*> in the exe's writable sections whose first objects hold their own index at kIndex.
// - UObject's Class: the field where the class Class points to itself, Name and Outer before it (Mass Effect LE +0x50,
//   +0x48, +0x40; Dishonored packs UObject to 4 bytes: +0x4C, +0x44, +0x3C).
// - Names, whichever reads "Class" for the class Class and identifiers for other objects:
//   - BioWare's SFXName (Mass Effect LE): an FName is a 29-bit byte offset into a name chunk plus a 3-bit chunk index;
//     the chunk bases are an array in the exe's writable sections. A name entry: u32 flags, an 8-byte hash link
//     (packed), then the text (+12).
//   - UE3's GNames (Dishonored): a TArray<FNameEntry*> indexed by the FName's index, the text at +0x14 in Dishonored.
// - UProperty's offset field: the one holding FMatrix's plane offsets (0, 0x10, 0x20, 0x30) on Core.Object.Matrix.*Plane.
// - Then by name: Engine.Engine.GamePlayers, Engine.Player.Actor, Engine.Actor.Rotation, Engine.Controller.Pawn,
//   Engine.Actor.WorldInfo, and the GameEngine object (or a subclass's: SFXEngine in Mass Effect LE 2). UStruct's
//   SuperField: the field through which the class PlayerController reaches Controller. Optional: WorldInfo.Pauser
//   (Mass Effect; Dishonored has none) and the bools WorldInfo.Paused / bPlayersOnly / bIsMenuLevel, with UBoolProperty's
//   bitmask field: the one where WorldInfo's bools sharing an offset hold distinct single bits.
// Each frame: GameEngine.GamePlayers[0].Actor (the controller); a gameplay view while it has a pawn and the world is a
// game level (WorldInfo.bIsMenuLevel off; Dishonored keeps it on in play, so its profile has [Unreal3] MenuLevel=0),
// paused while Pauser is set or WorldInfo.Paused (Dishonored's pause menu) or bPlayersOnly (BioWare's menus) is on. If
// any of this isn't found, the gyro goes to the game as right-stick input (the core's blind stick fallback).
// The game clamps Rotation's pitch itself: limit_turn learns where it stops, so pushing the gyro against the limit isn't
// taken for writes the game undid (the stick fallback started that way in Mass Effect LE 3, MODLOG 2026-10-30).
#include "unreal3.hpp"
#include "ue3_xinput.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace gyro4all::unreal3 {
namespace {

// UObject: Index (Mass Effect LE, Dishonored); the others are found (find_class_field, find_super_field).
constexpr uintptr_t kIndex = 0x08;
uintptr_t g_outer = 0, g_name = 0, g_class = 0;
uintptr_t g_super_field = 0;  // UStruct.SuperField
constexpr double kUnitsPerDegree = 65536.0 / 360.0;
constexpr ULONGLONG kRetryMs = 2000, kGiveUpMs = 60000;
// limit_turn: how long the pitch must stay put while turns ask for more to count as the game's limit, and how far from
// level it must be (a scene holding the camera near level isn't a limit).
constexpr ULONGLONG kStuckMs = 40;
constexpr double kMinLimit = 30;

std::wstring g_cooked;  // detect(): the cooked package folder found, relative to the game's root (for the log)

// <root>\<sub>\CookedPC* with <root> two levels above the exe's folder (Binaries\Win64). Returns "<sub>\<CookedPC...>".
std::wstring find_cooked(const std::wstring& exe_path) {
    const std::wstring dir = exe_path.substr(0, exe_path.find_last_of(L"\\/"));
    const std::wstring root = dir + L"\\..\\..";
    WIN32_FIND_DATAW sub{};
    HANDLE subs = FindFirstFileW((root + L"\\*").c_str(), &sub);
    if (subs == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sub.cFileName[0] == L'.') continue;
        WIN32_FIND_DATAW cooked{};
        HANDLE c = FindFirstFileW((root + L"\\" + sub.cFileName + L"\\CookedPC*").c_str(), &cooked);
        if (c == INVALID_HANDLE_VALUE) continue;
        do {
            if (cooked.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) found = std::wstring(sub.cFileName) + L"\\" + cooked.cFileName;
        } while (found.empty() && FindNextFileW(c, &cooked));
        FindClose(c);
    } while (found.empty() && FindNextFileW(subs, &sub));
    FindClose(subs);
    return found;
}

// ------------------------------------------------------------------ reflection

uintptr_t g_objects = 0;  // address of GObjects (TArray<UObject*>)
uintptr_t g_names = 0;    // address of the name chunk array (BioWare) or of GNames (TArray<FNameEntry*>)
enum class Names { Chunks, Array } g_names_kind = Names::Chunks;
uintptr_t g_name_text = 12;   // the text's offset in a name entry
uintptr_t g_prop_offset = 0;  // UProperty field holding the property's offset in its struct

struct Offsets {
    uint32_t game_players = 0, player_actor = 0, rotation = 0, pawn = 0, world_info = 0, pauser = 0;
} g_off;
// UBoolProperty's bitmask field (Mass Effect LE +0xD0, Dishonored +0xA4); a WorldInfo bool: offset and mask (0 = not
// found).
uintptr_t g_bool_mask = 0;
struct Bool {
    uint32_t offset = 0, mask = 0;
};
// bPlayersOnly: the pause menu and other full-screen menus freeze the world this way (Pauser stays empty, game time
// runs on). bIsMenuLevel: the main menu's world (entrymenu), whose controller has a pawn too. Paused: Dishonored's
// pause menu (no Pauser there).
Bool g_players_only, g_menu_level, g_paused;
uintptr_t g_engine = 0;  // the GameEngine object

uintptr_t object_at(int32_t i) {
    const uintptr_t data = mem::rdp(g_objects);
    return data ? mem::rdp(data + uintptr_t(i) * 8) : 0;
}

int32_t object_count() {
    int32_t n = 0;
    return mem::rd(g_objects + 8, n) ? n : 0;
}

// A name entry's text through the table at names, read as kind ("" if unreadable).
std::string name_text(uintptr_t names, Names kind, uintptr_t text_at, uint32_t value) {
    uintptr_t entry = 0;
    if (kind == Names::Chunks) {
        const uintptr_t chunk = mem::rdp(names + ((value >> 29) & 7) * 8, 1);
        entry = chunk ? chunk + (value & 0x1FFFFFFF) : 0;
    } else {
        const uintptr_t data = mem::rdp(names);
        int32_t num = 0;
        // Entries at any byte (Dishonored packs them back to back).
        entry = data && mem::rd(names + 8, num) && value < uint32_t(num) ? mem::rdp(data + uintptr_t(value) * 8, 1) : 0;
    }
    char text[64] = {};
    if (!entry || !mem::guarded_copy(text, entry + text_at, sizeof text - 1)) return {};
    return std::string(text, strnlen(text, sizeof text - 1));
}

// The text of an FName value ("" if unreadable).
std::string fname(uint32_t value) { return name_text(g_names, g_names_kind, g_name_text, value); }

std::string name_of(uintptr_t object) {
    uint32_t value = 0;
    return object && mem::rd(object + g_name, value) ? fname(value) : std::string{};
}

uintptr_t class_of(uintptr_t object) { return mem::rdp(object + g_class); }
uintptr_t outer_of(uintptr_t object) { return mem::rdp(object + g_outer); }

// Whether an object's class is base_name or derives from it (Mass Effect LE 2's engine is an SFXEngine).
bool is_a(uintptr_t object, const char* base_name) {
    uintptr_t c = class_of(object);
    for (int depth = 0; c && depth < 32; ++depth, c = g_super_field ? mem::rdp(c + g_super_field) : 0)
        if (name_of(c) == base_name) return true;
    return false;
}

// GObjects by shape: plausible counts, and the first objects hold their own index.
bool find_objects() {
    for (const mem::Range& r : mem::writable_sections()) {
        for (uintptr_t a = (r.begin + 7) & ~uintptr_t(7); a + 16 <= r.end; a += 8) {
            const uintptr_t data = mem::rdp(a);
            int32_t num = 0, max = 0;
            if (!data || !mem::rd(a + 8, num) || !mem::rd(a + 12, max) || num < 20000 || num > 5000000 || max < num ||
                max > num * 4)
                continue;
            int good = 0;
            for (int32_t i = 1; i <= 16; ++i) {
                int32_t index = -1;
                const uintptr_t o = mem::rdp(data + uintptr_t(i) * 8);
                if (o && mem::rd(o + kIndex, index) && index == i) ++good;
            }
            if (good >= 14) {
                g_objects = a;
                return true;
            }
        }
    }
    return false;
}

// UObject's Class field (4-byte steps: Dishonored packs UObject): the class Class points to itself there, and the
// first objects' classes are objects whose class is that one. Name and Outer are the two 8-byte fields before it.
bool find_class_field() {
    const int32_t n = std::min(object_count(), 20000);
    for (uintptr_t f = kIndex + 4; f < 0x100; f += 4) {
        uintptr_t class_class = 0;
        for (int32_t i = 0; i < n && !class_class; ++i)
            if (const uintptr_t o = object_at(i); o && mem::rdp(o + f) == o) class_class = o;
        if (!class_class) continue;
        int good = 0;
        for (int32_t i = 0; i < 64; ++i) {
            const uintptr_t o = object_at(i);
            const uintptr_t c = o ? mem::rdp(o + f) : 0;
            good += c && mem::rdp(c + f) == class_class;
        }
        if (good >= 56) {
            g_class = f, g_name = f - 8, g_outer = f - 16;
            log("ue3: UObject Class +0x%llx, Name +0x%llx, Outer +0x%llx (%d of the first 64 objects' classes are classes)",
                (unsigned long long)g_class, (unsigned long long)g_name, (unsigned long long)g_outer, good);
            return true;
        }
    }
    return false;
}

// A non-empty identifier.
bool identifier(const std::string& text) {
    if (text.empty()) return false;
    for (char c : text)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

// The name table: where the class Class's name reads "Class", and other objects' names (in every chunk they use, with
// BioWare's) read as identifiers (Mass Effect LE 1 has a copy of chunk 0's base elsewhere in .data, before the array).
bool find_names() {
    uintptr_t class_class = 0;
    std::vector<uint32_t> samples;  // a few names from each chunk in use (BioWare's), spread over GObjects
    int per_chunk[8] = {};
    // Spread over all of GObjects: the first objects' names are all in chunk 0, and Mass Effect LE 2 has a decoy array
    // there whose chunk 0 is right and chunk 1 isn't.
    const int32_t n = object_count(), step = n / 20000 + 1;
    for (int32_t i = 0; i < n; i += step) {
        const uintptr_t o = object_at(i);
        uint32_t v = 0;
        if (o && mem::rd(o + g_name, v) && per_chunk[(v >> 29) & 7]++ < 8) samples.push_back(v);
    }
    for (int32_t i = 0; i < n && i < 20000 && !class_class; ++i) {  // every object, not the samples' stride
        const uintptr_t o = object_at(i);
        if (o && class_of(o) == o) class_class = o;
    }
    uint32_t value = 0;
    if (!class_class || !mem::rd(class_class + g_name, value)) return false;
    // Most samples, not all: some objects have names that aren't identifiers (Mass Effect LE 1: "<uninitialized>").
    auto fits = [&](uintptr_t a, Names kind, uintptr_t text_at) {
        if (name_text(a, kind, text_at, value) != "Class") return false;
        if (kind == Names::Chunks) {
            // Every chunk the names use must exist and be its own: a decoy array can repeat chunk 0 in slot 1 (Mass Effect
            // LE 3), and what it reads there are fragments of other names, which still look like identifiers.
            uintptr_t seen[8] = {};
            for (uint32_t v : samples) {
                const uintptr_t slot = (v >> 29) & 7, chunk = mem::rdp(a + slot * 8);
                if (!chunk || (std::find(seen, seen + 8, chunk) != seen + 8 && seen[slot] != chunk)) return false;
                seen[slot] = chunk;
            }
        }
        size_t good = 0;
        for (uint32_t v : samples) good += identifier(name_text(a, kind, text_at, v));
        if (good * 4 < samples.size() * 3) return false;
        g_names = a, g_names_kind = kind, g_name_text = text_at;
        return true;
    };
    const uintptr_t chunk_slot = ((value >> 29) & 7) * 8;
    for (const mem::Range& r : mem::writable_sections()) {
        for (uintptr_t a = (r.begin + 7) & ~uintptr_t(7); a + 64 <= r.end; a += 8) {
            // Cheap filters first: a pointer where the chunk would be, or a TArray holding the class Class's index.
            if (mem::rdp(a + chunk_slot, 1) && fits(a, Names::Chunks, 12)) return true;
            int32_t num = 0, max = 0;
            if (mem::rdp(a) && mem::rd(a + 8, num) && mem::rd(a + 12, max) && value < uint32_t(num) && num <= max &&
                max <= num * 4)
                for (uintptr_t text_at : {0x10, 0x14, 0x18, 0x0C})
                    if (fits(a, Names::Array, text_at)) return true;
        }
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("ue3: name table not found (the class Class at %s, its name value 0x%x, %zu sample names)",
            mem::module_offset(class_class).c_str(), value, samples.size());
    }
    return false;
}

// UStruct.SuperField: the field (4-byte steps) through which the class PlayerController reaches Controller.
void find_super_field(uintptr_t player_controller) {
    for (uintptr_t f = g_class + 8; f < 0x120 && player_controller && !g_super_field; f += 4)
        if (name_of(mem::rdp(player_controller + f)) == "Controller") g_super_field = f;
}

// UBoolProperty's bitmask field: where the given bools hold single bits, distinct among those sharing an offset (at
// least two must share one).
void find_bool_mask(const std::vector<uintptr_t>& bools) {
    std::vector<std::pair<int32_t, uintptr_t>> at;  // (offset, property)
    for (uintptr_t b : bools)
        if (int32_t offset = 0; mem::rd(b + g_prop_offset, offset)) at.push_back({offset, b});
    bool shared = false;
    for (size_t i = 0; i < at.size(); ++i)
        for (size_t j = i + 1; j < at.size(); ++j) shared = shared || at[i].first == at[j].first;
    if (!shared) return;
    for (uintptr_t f = g_prop_offset + 4; f < 0x120 && !g_bool_mask; f += 4) {
        bool ok = true;
        std::vector<std::pair<int32_t, uint32_t>> seen;  // (offset, bits so far)
        for (const auto& [offset, b] : at) {
            uint32_t m = 0;
            ok = ok && mem::rd(b + f, m) && m && !(m & (m - 1));
            if (!ok) break;
            auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.first == offset; });
            if (it == seen.end()) seen.push_back({offset, m});
            else if (it->second & m) ok = false;
            else it->second |= m;
        }
        if (ok) g_bool_mask = f;
    }
}

// The rest by name, in one pass over GObjects.
bool find_members() {
    struct Want {
        const char* owner;
        const char* name;
        uint32_t* out;
        bool needed;
    } wants[] = {{"Engine", "GamePlayers", &g_off.game_players, true}, {"Player", "Actor", &g_off.player_actor, true},
                 {"Actor", "Rotation", &g_off.rotation, true},         {"Controller", "Pawn", &g_off.pawn, true},
                 {"Actor", "WorldInfo", &g_off.world_info, true},      {"WorldInfo", "Pauser", &g_off.pauser, false}};
    uintptr_t planes[4] = {};  // Core.Object.Matrix.XPlane..WPlane
    std::vector<std::pair<uintptr_t, uint32_t*>> props;
    uintptr_t players_only = 0, players_only_pending = 0, menu_level = 0, paused = 0, player_controller = 0;
    std::vector<uintptr_t> engines;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t o = object_at(i);
        if (!o) continue;
        const std::string cls = name_of(class_of(o));
        if (!cls.ends_with("Property")) {
            if (cls.ends_with("Engine") && !name_of(o).starts_with("Default__")) engines.push_back(o);
            else if (!player_controller && cls == "Class" && name_of(o) == "PlayerController") player_controller = o;
            continue;
        }
        const std::string name = name_of(o);
        const std::string owner = name_of(outer_of(o));
        if (owner == "Matrix" && name.size() == 6 && name.ends_with("Plane") && std::strchr("XYZW", name[0]))
            planes[std::strchr("XYZW", name[0]) - "XYZW"] = o;
        for (const Want& w : wants)
            if (name == w.name && owner == w.owner) props.push_back({o, w.out});
        if (owner == "WorldInfo" && cls == "BoolProperty") {
            if (name == "bPlayersOnly") players_only = o;
            else if (name == "bPlayersOnlyPending") players_only_pending = o;
            else if (name == "bIsMenuLevel") menu_level = o;
            else if (name == "Paused") paused = o;
        }
    }
    // UProperty's offset field: FMatrix's planes are at 0, 0x10, 0x20, 0x30.
    g_prop_offset = 0;
    for (uintptr_t field = 0x60; field < 0x100 && !g_prop_offset && planes[3]; field += 4) {
        bool all = true;
        for (int k = 0; k < 4 && all; ++k) {
            int32_t v = -1;
            all = planes[k] && mem::rd(planes[k] + field, v) && v == k * 0x10;
        }
        if (all) g_prop_offset = field;
    }
    if (!g_prop_offset) return false;
    if (!g_super_field) find_super_field(player_controller);
    g_engine = 0;
    for (uintptr_t e : engines)
        if (!g_engine && is_a(e, "GameEngine")) g_engine = e;
    for (auto& [prop, out] : props) {
        int32_t v = 0;
        if (mem::rd(prop + g_prop_offset, v) && v > 0) *out = uint32_t(v);
    }
    for (const Want& w : wants)
        if (!*w.out && w.needed) {
            log("ue3: property %s.%s not found", w.owner, w.name);
            return false;
        }
    // Optional: without them pauses or menus just aren't seen.
    std::vector<uintptr_t> world_bools;
    for (uintptr_t b : {players_only, players_only_pending, menu_level, paused})
        if (b) world_bools.push_back(b);
    if (!g_bool_mask) find_bool_mask(world_bools);
    auto read_bool = [](uintptr_t prop, const char* name) {
        Bool b;
        int32_t offset = 0;
        uint32_t mask = 0;
        if (prop && g_bool_mask && mem::rd(prop + g_prop_offset, offset) && offset > 0 &&
            mem::rd(prop + g_bool_mask, mask) && mask && !(mask & (mask - 1)))
            b = {uint32_t(offset), mask};
        else
            log("ue3: WorldInfo.%s not found: no pause or menu detection through it", name);
        return b;
    };
    g_players_only = read_bool(players_only, "bPlayersOnly");
    g_menu_level = read_bool(menu_level, "bIsMenuLevel");
    g_paused = read_bool(paused, "Paused");
    return g_engine != 0;
}

// ------------------------------------------------------------------ the engine

enum class Stage { Waiting, Ready, Failed };

class Unreal3Engine final : public Engine {
public:
    const char* name() const override { return "Unreal3"; }

    bool detect(const std::wstring& exe_path) const override {
        g_cooked = find_cooked(exe_path);
        return !g_cooked.empty();
    }

    void controllers_changed() override { recheck_controllers(); }

    void configure() override {
        log("ue3: cooked packages in %s", narrow(g_cooked).c_str());
        // Dishonored keeps WorldInfo.bIsMenuLevel on in play: its profile turns the menu check off.
        use_menu_level_ = settings::profile_int(L"Unreal3", L"MenuLevel", 1) != 0;
        if (!use_menu_level_) log("ue3: WorldInfo.bIsMenuLevel ignored (profile: [Unreal3] MenuLevel=0)");
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        if (stage_ == Stage::Waiting) resolve();
        if (stage_ == Stage::Failed) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
            return f;
        }
        if (stage_ != Stage::Ready) return f;
        const uintptr_t players = mem::rdp(g_engine + g_off.game_players);
        int32_t count = 0;
        const uintptr_t player = players && mem::rd(g_engine + g_off.game_players + 8, count) && count > 0 ? mem::rdp(players) : 0;
        const uintptr_t pc = player ? mem::rdp(player + g_off.player_actor) : 0;
        if (pc != pc_) {
            pc_ = pc;
            acc_pitch_ = acc_yaw_ = 0;
            top_ = 90, bottom_ = -90, stuck_dir_ = 0;  // another controller (a level load): its limits are learnt again
            log("ue3: player controller %s", pc ? (name_of(class_of(pc)) + " " + name_of(pc)).c_str() : "none");
        }
        if (!pc) return f;
        const uintptr_t world = mem::rdp(pc + g_off.world_info);
        const uintptr_t pauser = world && g_off.pauser ? mem::rdp(world + g_off.pauser) : 0;
        const bool players_only = world_bool(world, g_players_only), paused = world_bool(world, g_paused);
        const bool menu_level = use_menu_level_ && world_bool(world, g_menu_level);
        if (players_only != players_only_ || menu_level != menu_level_) {
            players_only_ = players_only;
            menu_level_ = menu_level;
            log("ue3: world %s%s", menu_level ? "is a menu level" : "is a game level",
                players_only ? ", frozen (bPlayersOnly: a menu)" : "");
        }
        const uintptr_t pawn = mem::rdp(pc + g_off.pawn);
        if (pawn != pawn_) {
            pawn_ = pawn;
            pawn_name_ = pawn ? name_of(class_of(pawn)) : std::string{};
        }
        f.in_game = true;
        f.paused = pauser != 0 || players_only || paused;
        f.view_id = pawn && !menu_level ? 1 : 0;
        f.target = pawn ? pawn_name_ : "no pawn";
        return f;
    }

    // The game's pitch limit isn't read from it (Camera.ViewPitchMin/Max aren't what every game clamps to): a pitch past
    // kMinLimit that stays put for kStuckMs while every turn asks for more is taken as the limit there, and the turn
    // stops at it from then on (raised again if the pitch ever goes past it). While it's being learnt, the turn already
    // stops wherever the pitch is stuck: the stick fallback's 8 undone frames come before kStuckMs at 165 frames/s (Mass
    // Effect LE 3: it started each time, a few ms before the limit was learnt).
    void limit_turn(double& pitch, double& /*yaw*/) override {
        int32_t r[3] = {};
        if (stage_ != Stage::Ready || !pc_ || !mem::rd(pc_ + g_off.rotation, r)) return;
        const double up = int16_t(r[0] & 0xFFFF) / kUnitsPerDegree;
        const ULONGLONG now = steady_ms();
        const bool recent = now - asked_at_ < 100;
        const bool still = std::abs(up - last_up_) < 0.02;
        const int pushing = !recent || !still                                    ? 0
                            : asked_up_ > up + 0.03 && up > kMinLimit            ? 1
                            : asked_up_ < up - 0.03 && up < -kMinLimit           ? -1
                                                                                 : 0;
        if (!pushing || pushing != stuck_dir_) {
            stuck_dir_ = pushing;
            stuck_since_ = now;
        } else if (now - stuck_since_ >= kStuckMs) {
            if (pushing > 0 && up < top_) {
                top_ = up;
                log("ue3: the view stops at %.1f degrees up: the gyro's pitch stops there", top_);
            } else if (pushing < 0 && up > bottom_) {
                bottom_ = up;
                log("ue3: the view stops at %.1f degrees down: the gyro's pitch stops there", -bottom_);
            }
        }
        if (up > top_) top_ = up;
        if (up < bottom_) bottom_ = up;
        // What the turn asks for, before any trim: a push stays a push on the next frames.
        asked_up_ = up + pitch;
        pitch = std::clamp(pitch, bottom_ - up, top_ - up);
        if (pushing > 0) pitch = std::min(pitch, 0.0);
        if (pushing < 0) pitch = std::max(pitch, 0.0);
        asked_at_ = now;
        last_up_ = up;
    }

    // Gyro degrees (pitch up, yaw right) to rotator units, the fractions kept for the next frame.
    bool apply(double pitch, double yaw) override {
        int32_t r[3] = {};
        if (stage_ != Stage::Ready || !pc_ || !mem::rd(pc_ + g_off.rotation, r)) return false;
        acc_pitch_ += pitch * kUnitsPerDegree;
        acc_yaw_ += yaw * kUnitsPerDegree;
        const int32_t dp = int32_t(acc_pitch_), dy = int32_t(acc_yaw_);
        acc_pitch_ -= dp;
        acc_yaw_ -= dy;
        if (!dp && !dy) return true;
        r[0] = (r[0] + dp) & 0xFFFF;  // the game keeps pitch in 0..65535 and limits it itself
        r[1] += dy;
        return mem::guarded_store(pc_ + g_off.rotation, r, sizeof(int32_t) * 2);
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera) override {
        int32_t r[3] = {};
        if (stage_ != Stage::Ready || !pc_ || !mem::rd(pc_ + g_off.rotation, r)) return false;
        pitch = int16_t(r[0] & 0xFFFF) / kUnitsPerDegree;
        yaw = r[1] / kUnitsPerDegree;
        camera = pc_;
        return true;
    }

    bool sees_menus() const override { return stage_ != Stage::Failed; }

    std::string status_scene() const override {
        if (stage_ == Stage::Failed) return "ue3: no camera hook, gyro as right stick";
        if (stage_ != Stage::Ready) return "ue3: looking for the engine";
        int32_t r[3] = {};
        if (!pc_ || !mem::rd(pc_ + g_off.rotation, r)) return "ue3: no player controller";
        return std::format("ue3: {} rotation pitch {:.1f} yaw {:.1f}", pawn_ ? pawn_name_ : "no pawn",
                           int16_t(r[0] & 0xFFFF) / kUnitsPerDegree, r[1] / kUnitsPerDegree);
    }

private:
    // Every kRetryMs until found (the objects appear as the game loads), the stick fallback after kGiveUpMs.
    void resolve() {
        const ULONGLONG now = GetTickCount64();
        if (!first_try_) first_try_ = now;
        if (last_try_ && now - last_try_ < kRetryMs) return;
        last_try_ = now;
        const auto start = std::chrono::steady_clock::now();
        const bool ok = (g_objects || find_objects()) && (g_class || find_class_field()) && (g_names || find_names()) &&
                        find_members();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (ok) {
            stage_ = Stage::Ready;
            log("ue3: GObjects at %s (%d objects), UObject Outer +0x%llx Name +0x%llx Class +0x%llx, %s at %s (text +0x%llx), "
                "property offset field +0x%llx, SuperField +0x%llx, bool mask +0x%llx, %.0f ms",
                mem::module_offset(g_objects).c_str(), object_count(), (unsigned long long)g_outer, (unsigned long long)g_name,
                (unsigned long long)g_class, g_names_kind == Names::Chunks ? "name chunks" : "GNames",
                mem::module_offset(g_names).c_str(), (unsigned long long)g_name_text, (unsigned long long)g_prop_offset,
                (unsigned long long)g_super_field, (unsigned long long)g_bool_mask, ms);
            log("ue3: GamePlayers +0x%x, Player.Actor +0x%x, Actor.Rotation +0x%x, Controller.Pawn +0x%x, Actor.WorldInfo +0x%x, "
                "WorldInfo.Pauser +0x%x, Paused +0x%x/0x%x, bPlayersOnly +0x%x/0x%x, bIsMenuLevel +0x%x/0x%x, GameEngine %s",
                g_off.game_players, g_off.player_actor, g_off.rotation, g_off.pawn, g_off.world_info, g_off.pauser,
                g_paused.offset, g_paused.mask, g_players_only.offset, g_players_only.mask, g_menu_level.offset,
                g_menu_level.mask, mem::module_offset(g_engine).c_str());
        } else if (now - first_try_ > kGiveUpMs) {
            stage_ = Stage::Failed;
            log("ue3: engine not found (GObjects %s, names %s, property offset field %s): the gyro goes to the game as "
                "right-stick input", g_objects ? "found" : "missing", g_names ? "found" : "missing", g_prop_offset ? "found" : "missing");
        }
    }

    static ULONGLONG steady_ms() {
        return ULONGLONG(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
    }

    static bool world_bool(uintptr_t world, const Bool& b) {
        uint32_t word = 0;
        return world && b.mask && mem::rd(world + b.offset, word) && (word & b.mask);
    }

    Stage stage_ = Stage::Waiting;
    bool players_only_ = false, menu_level_ = false, use_menu_level_ = true;
    ULONGLONG first_try_ = 0, last_try_ = 0;
    uintptr_t pc_ = 0, pawn_ = 0;
    std::string pawn_name_;
    double acc_pitch_ = 0, acc_yaw_ = 0;
    // limit_turn: the pitch limits as seen (degrees, up positive) and the pitch the last turn asked for.
    double top_ = 90, bottom_ = -90, asked_up_ = 0, last_up_ = 0;
    ULONGLONG asked_at_ = 0, stuck_since_ = 0;
    int stuck_dir_ = 0;
};

}  // namespace

Engine& engine() {
    static Unreal3Engine instance;
    return instance;
}

}  // namespace gyro4all::unreal3
