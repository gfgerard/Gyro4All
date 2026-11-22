#pragma once
// The game profile's Unreal camera views (Gyro4All\profiles\<exe stem>.ini), read at start and again whenever the file
// changes, and the views the camera probe learns during a session, added to it. No game memory access, so it is
// unit-tested on its own (tests/profile_tests.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::unreal {

// A state of the game read from a reflected property: PauseConditions= ([Gyro4All] of the profile, states that count as
// a pause, for scripted scenes the engine can't tell from play; the gyro pauses while any holds) and a view's
// ActiveWhen=. For example:
//   PauseConditions=Pawn.CharacterMovement.MovementMode==5, PC.bCinematicMode
// Each is Root.Sub...Property (Root = PC, Pawn, Camera or ViewTarget, steps as in Object=), alone (the value is true or
// non-zero) or followed by == != < <= > >= and a number, true or false. The property is a bool, a number, a byte or
// enum (its value as a number) or an object (1 when set).
struct Condition {
    enum class Op { NonZero, Eq, Ne, Lt, Le, Gt, Ge };
    std::string text;                 // as written (trimmed), for the log
    std::vector<std::string> object;  // Root.Sub... (the object holding the property)
    std::string property;
    Op op = Op::NonZero;
    double value = 0;
    // runtime (ue_engine.cpp)
    uintptr_t missing_class = 0;  // root class whose path couldn't be read, logged once per class
    bool held = false;

    bool holds(double v) const;
};

// Parses one condition. False with `error` set for an empty text, a path without a root and a property, an unknown
// root, a character a path can't have, a missing value or one that isn't a number, true or false.
bool parse_condition(const std::string& text, Condition& out, std::string& error);

// A gameplay view from the game profile. The default view (id 1) turns the PlayerController's
// ControlRotation; other views add the deltas to a rotator property of the camera view target.
// Any view can name another object (Object=Root.Sub..., Root = PC, Pawn, Camera or ViewTarget; each Sub is an
// object property or a subobject name) and, for non-reflected C++ state, raw rotator offsets in it.
struct View {
    uint32_t id = 1;
    std::string key = "Default", label = "Exploration";
    std::string view_target_class;  // active when the view target's class chain has this class
    // Match=Camera: active while the camera follows this view's rotation, among the views for the same view target
    // (see core/camera_match.hpp). For games that turn the same view target's camera through different rotations,
    // e.g. PAYDAY 3: ControlRotation in normal play, a camera modifier's clamped rotation during some actions.
    bool match_camera = false;
    std::string property;           // rotator property on the view object ("" = ControlRotation)
    std::vector<std::string> object;  // path to the view object (empty = PC for the default view, else ViewTarget)
    std::vector<int32_t> offsets;     // raw rotator offsets in the view object (instead of Property)
    int32_t rotator_size = 24;        // 24 = double (UE5), 12 = float (UE4)
    // Matrix=1: each offset is a float rotation matrix (FMatrix rows forward, right, up) instead of a rotator, for C++
    // cameras that keep the view that way (Days Gone: Camera.@BendPlayerCamFollow_C +0x120). RotatorSize is ignored.
    bool matrix = false;
    // Direction=1: each offset is a float forward vector (x, y, z) instead of a rotator: pitch and yaw are its angles, no
    // roll. For cameras that aim along a stored direction (Stellar Blade's aim: the character's +0x1534). RotatorSize is
    // ignored.
    bool direction = false;
    // ScreenPoint=<degrees across>,<degrees down>: each offset is a float screen point (x, y, 0..1 from the top left)
    // instead of a rotator, moved as if the screen spanned that many degrees; for crosshairs that move on a fixed
    // camera (Stellar Blade's cover minigame: the character's +0x159C). 0 = not a screen point. The camera isn't
    // expected to follow it.
    double screen_width = 0, screen_height = 0;
    // ActiveList=<offset> (with an Object= ending in @Class, where Class may list several: @A|B|C): the object before
    // that step keeps a TArray<UObject*> at that offset, newest first, whose first entry (past ActiveListSkip= classes)
    // is the active one. The view is active while that entry is of one of its classes, and turns it. For games with one
    // camera object per mode (Days Gone: BendCamManager's camera stack +0x35D0; BendCamTransition entries while blending).
    int32_t active_list = -1;
    std::vector<std::string> active_skip;
    uintptr_t active_object = 0;  // runtime: the list's entry this view turns (0 = none)
    // OutputMatrix=<offset> (with Matrix=1): the matrix the game shows, built from the one written. A written pitch beyond
    // it (the game's pitch limit) is brought back to it first, so the gyro doesn't wind up past the limit.
    int32_t output_matrix = -1;
    // WorldRotation=1: Property is a scene component's RelativeRotation with absolute rotation; its world rotation
    // (ComponentToWorld) is written too, because the game turns the component from it (Severed Steel's camera).
    bool world_rotation = false;
    const char* world_why = nullptr;  // last reason it couldn't be applied (logged when it changes)
    // Function=<name>: the turn is applied by calling this reflected function of the view object (its first Rotator
    // parameter gets the turn, bTeleport is true) instead of writing memory; Property= is only read (status, follow
    // check). For physics bodies that undo memory writes (EVERSPACE 2's ship: Function=K2_AddLocalRotation on its root).
    std::string function;
    const char* function_why = nullptr;  // last reason the call failed (logged when it changes)
    uint32_t parent = 0;            // settings inheritance
    // PitchView=<id>: this view turns yaw only, its pitch goes into that view's rotation. For cameras that split the
    // axes over two objects (Dead Island 2: yaw from the pawn's capsule, pitch from ControlRotation). 0 = both here.
    uint32_t pitch_view = 0;
    // PitchObject= with PitchProperty= (or PitchOffsets= and PitchRotatorSize=): this view turns yaw only, its pitch
    // goes into that rotation (same syntax as Object=, Property=, Offsets=, RotatorSize=; no PitchObject = this view's
    // own object). For first-person cameras that keep the two axes in two objects: Amid Evil turns the yaw with
    // ControlRotation (the character) and the pitch with the camera component's RelativeRotation. Wins over PitchView=.
    std::vector<std::string> pitch_object;
    std::string pitch_property;
    std::vector<int32_t> pitch_offsets;
    int32_t pitch_rotator_size = 24;
    bool has_pitch_rotation() const { return !pitch_property.empty() || !pitch_offsets.empty(); }
    // Optional: scalar properties written whenever the gyro moves this view (e.g. to cancel a camera
    // auto-recentering that the game only cancels for stick input). "Name:value" is on the view object;
    // "Root.Sub.Name:value" on another object, with the same path syntax as Object= (Life is Strange:
    // OnGyro=PC.@TP_Walk_Manual_C.SmoothedAutoManualRatio:1).
    struct OnGyro {
        std::vector<std::string> object;  // empty = the view object
        std::string property;
        double value = 0;
        bool warned = false;
    };
    std::vector<OnGyro> on_gyro;
    // Camera recenter (GyroLib's camera.recenter_button): Recenter=0 withdraws it in this view; RecenterPitch= is the
    // level pitch in the view's own rotator (degrees, default 0).
    bool recenter = true;
    double recenter_pitch = 0;
    // Unzoomed horizontal FOV used by zoom compensation; 0 reads PlayerCameraManager.DefaultFOV.
    double zoom_reference_fov = 0;
    // ActiveWhen=<condition> (Condition syntax): the view is used while it holds, ahead of ViewTargetClass and
    // Match=Camera views, from the frame it starts holding. For a mode the game flags (Stellar Blade's aim:
    // ActiveWhen=Pawn.@SBCameraComponent.bCurrentCameraTPSMode).
    bool has_active_when = false;
    Condition active_when;
    // ControlYaw=1: after each gyro turn, PC.ControlRotation's yaw is set to this view's yaw, for cameras turned through
    // another rotation while the character moves along ControlRotation, which the game syncs from the camera only on
    // stick input (Stellar Blade's boom).
    bool control_yaw = false;
    // Diagnostics: log these properties whenever they change; Dump=1 logs all properties once.
    std::vector<std::string> watch;
    bool dump = false, dumped = false;
    std::vector<std::string> watched_values;
};

// The profile has views of its own (a [View.Default] section or a Views= list).
bool has_views(const std::wstring& path);

// The default view first, then every valid view in Views= (invalid ones are logged and skipped). A missing file gives
// the default view alone.
std::vector<View> load_views(const std::wstring& path);

// The profile's PauseConditions= (invalid ones are logged and skipped). None when the profile has none.
std::vector<Condition> load_pause_conditions(const std::wstring& path);

// A rotation the camera probe verified, to add to the profile as a Match=Camera view.
struct LearnedView {
    std::string object;    // profile path ("Camera.@SBZClampCameraRotationModifier")
    std::string property;  // reflected rotator ("" = raw offsets)
    std::vector<int32_t> offsets;
    int32_t rotator_size = 24;
    bool world_rotation = false;
    uint32_t pitch_view = 0;  // PitchView=: its live test moved yaw only, pitch goes to that view
    // PitchObject= / PitchProperty= (or PitchOffsets= / PitchRotatorSize=): its live test moved yaw only, and this
    // rotation moved the camera's pitch alone (wins over pitch_view). Empty pitch_object: not set.
    std::string pitch_object, pitch_property;
    std::vector<int32_t> pitch_offsets;
    int32_t pitch_rotator_size = 24;
    std::string label;  // tab label in the settings panel
    std::string note;   // a comment written above the section (where and when it was learned)
};

// The PitchObject= ... lines for a learned pitch rotation (learned.pitch_object), each ending in `eol`; "" if none.
std::string pitch_keys_text(const LearnedView& learned, const char* eol);

// Sets the pitch rotation of the profile's existing view [View.<key>] ("Default" for the default view, created if
// missing): its PitchObject= and PitchProperty= or PitchOffsets= / PitchRotatorSize= (the other keys of that kind are
// removed). The rest of the file is kept. For a probe that found the pitch of a view whose yaw already works.
bool set_pitch_rotation(const std::wstring& path, const std::string& key, const LearnedView& pitch);

// The view already turns that rotation (same object path, property or raw offsets).
bool same_rotation(const View& view, const LearnedView& learned);

enum class AddResult { Added, Duplicate, Failed };

// Adds `learned` to the profile at `path` (created if missing) as [View.Auto<id>] with Match=Camera and Parent=1, and
// appends it to Views= (in [Gyro4All]). The id is the lowest free one from 2, skipping `reserved` (the core's
// sensitivity profiles). Everything else in the file is kept. Duplicate when a view in it already turns that rotation.
AddResult add_view(const std::wstring& path, const std::string& exe_stem, const LearnedView& learned,
                   const std::vector<uint32_t>& reserved, uint32_t& id, std::string& key);

}  // namespace gyro4all::unreal
