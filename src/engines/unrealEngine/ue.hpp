#pragma once
// Minimal, engine-version-tolerant access to Unreal's reflection data (UE 4.25+ / UE5):
// - GUObjectArray and FNamePool are found by their shape in the exe's writable sections,
//   so no per-game byte signatures are needed;
// - UStruct/FField/FProperty offsets are detected at runtime against known engine classes;
// - objects and properties are then reached by name (GEngine -> world -> player -> ...).
// Every memory read is guarded: a bad pointer returns "not found" instead of crashing the game.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gyro4all::ue {

using Obj = uintptr_t;  // UObject* (0 = none)

struct Prop {
    uintptr_t field = 0;      // FProperty*
    int32_t offset = -1;      // Offset_Internal
    int32_t element_size = 0;
    std::string type;         // FFieldClass name, e.g. "StructProperty", "BoolProperty"
    explicit operator bool() const { return field != 0 && offset >= 0; }
};

// One-time discovery. Cheap to call repeatedly; returns true once everything was found.
bool init();
bool ready();

std::string name_of(Obj o);              // object or class short name ("BP_PlayerCarNew_C")
Obj class_of(Obj o);
bool is_a(Obj o, const char* class_name);  // class chain contains class_name (exact match)
bool is_a_like(Obj o, const char* part);   // a class name in the chain contains part (case-insensitive)
bool is_cdo(Obj o);

// Objects
Obj find_instance(const char* class_name);  // first non-CDO object whose class chain has class_name
Obj outer_of(Obj o);
Obj find_subobject(Obj outer, const char* name);  // object named `name` whose Outer is `outer` (full scan)
// First object whose Outer is `outer` and whose class chain has class_name (full scan). For subobjects whose
// instance name changes every session, e.g. "TP_Walk_Manual_C_2147462850".
Obj find_subobject_of_class(Obj outer, const char* class_name);

// Properties (class chain lookup, cached per class)
Prop find_prop(Obj cls_or_struct, const char* name);
Prop prop_of(Obj o, const char* name);       // lookup on o's class
Obj read_obj(Obj o, const char* name);       // object property value (0 if missing)
bool read_bool(Obj o, const char* name, bool fallback);
Obj struct_type(const Prop& p);              // UScriptStruct* of a StructProperty

// Rotators are float (UE4) or double (UE5 large world coordinates); element_size tells which.
bool add_rotator(uintptr_t address, int32_t element_size, double pitch, double yaw);
bool read_rotator(uintptr_t address, int32_t element_size, double& pitch, double& yaw, double& roll);

// Scene components with absolute rotation (their RelativeRotation is the world rotation) keep that rotation twice:
// in RelativeRotation and in the non-reflected ComponentToWorld transform, which is what game code reads back
// (Severed Steel turns its FirstPersonCamera from it every frame). Turning such a component means writing both.
// `relative` is the component's RelativeRotation property. On failure `why` says why (nothing was written).
bool add_world_rotation(Obj component, const Prop& relative, double pitch, double yaw, const char*& why);
// The component's ComponentToWorld rotation holds its RelativeRotation (absolute rotation, layout found).
bool has_world_rotation(Obj component, const Prop& relative);

// Diagnostics / profile helpers for scalar properties (Float, Double, Int, Byte, Bool).
void dump_props(Obj o, const char* stop_at_class);  // log every property down to (not including) stop_at_class
std::string value_string(Obj o, const char* name);  // "" if missing or unsupported type
bool write_number(Obj o, const char* name, double value);

// Probe: snapshot every float/double/rotator (structs nested up to 3 levels) of the roots and all
// objects whose Outer chain reaches a root (components, subobjects); the diff logs what changed.
// Labels are profile paths ("Pawn.View(Class)"): the part before "(" works as Object=.
size_t probe_snapshot(const std::vector<std::pair<std::string, Obj>>& roots);
// Logs what changed since the snapshot (verbose: every changed value, as for F8) and scores the rotations
// that followed the camera: they now hold its pitch and yaw, or changed by exactly the camera's turn (a rotation
// kept with an offset or a small lag, e.g. Hogwarts Legacy's camera stack). Returns the number of scored diffs so
// far (0 = not scored).
int probe_diff(bool verbose);

// Live test verdicts (the probe writes a small turn into a candidate and watches the camera).
// kTurnsInPlace: it turned the view, but the camera didn't move the way the stick moves it (a third-person camera
// orbits around the character; this one turned on the spot: a free camera, Hogwarts Legacy's camera component).
enum ProbeVerdict { kUntested = 0, kMovesCamera = 1, kOverwritten = -1, kNoEffect = -2, kUntestable = -3, kInconsistent = -4,
                    kTurnsInPlace = -5 };
// Rotations that followed the camera in most scored diffs (at least two), one per object (all raw rotators of an object
// together, as a profile writes them) or per reflected property. The ones that moved the camera in the live test
// first (pitch followed too, then also turns the character, first), then usable ones, then the ones the passive
// rules call outputs (tested too: the live test decides).
struct ProbeGroup {
    std::string id;        // stable key for probe_verdict
    std::string object;    // profile path ("ViewTarget.BehaviorStack")
    std::string cls, property;
    std::vector<int32_t> offsets;  // raw rotators (pitch, yaw, roll) when property is ""
    int size = 0;          // 12 float / 24 double rotator
    int hits = 0, exact = 0;  // scored diffs it followed / of which with the camera's exact values
    bool world = false;    // RelativeRotation with absolute rotation: write with WorldRotation=1
    std::string why_not;   // passive reason it looks like an output ("" = usable)
    int verdict = kUntested;
    std::string note;      // live test details
    bool pitch_followed = false, turns_pawn = false;  // live test: pitch writes moved the camera too / it turned the character
};
std::vector<ProbeGroup> probe_groups();
void probe_verdict(const std::string& id, int verdict, const std::string& note, bool pitch_followed = false,
                   bool turns_pawn = false);
// Forgets the snapshot, the candidates and the verdicts: a probe started again during play looks at the camera of
// that moment only.
void probe_reset();

// Profile (INI text) from the scored diffs, "" until there are at least two. summary = one-line pick. A group
// that moved the camera in the live test is picked first and the text says so (verified = true); otherwise the
// pick is the passive best guess, marked NOT VERIFIED.
// as_profile: the header for the game's own profile (the game had none), else for <exe>.suggested.ini.
std::string probe_suggestion(const std::string& exe_stem, std::string& summary, bool as_profile, bool& verified);

// The camera's final view rotation (PlayerCameraManager CameraCachePrivate.POV.Rotation).
bool camera_pov(Obj camera, double& pitch, double& yaw);
// The camera's final view location (...POV.Location), in world units.
bool camera_pov_location(Obj camera, double location[3]);

// Raw guarded reads
bool read_ptr(uintptr_t address, uintptr_t& out);
bool read_i32(uintptr_t address, int32_t& out);

}  // namespace gyro4all::ue
