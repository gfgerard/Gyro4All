#pragma once
// The game profile's Unreal camera views (Gyro4All\profiles\<exe stem>.ini), read at start and again whenever the file
// changes, and the views the camera probe learns during a session, added to it. No game memory access, so it is
// unit-tested on its own (tests/profile_tests.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::unreal {

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
    uint32_t parent = 0;            // settings inheritance
    // PitchView=<id>: this view turns yaw only, its pitch goes into that view's rotation. For cameras that split the
    // axes over two objects (Dead Island 2: yaw from the pawn's capsule, pitch from ControlRotation). 0 = both here.
    uint32_t pitch_view = 0;
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

// A rotation the camera probe verified, to add to the profile as a Match=Camera view.
struct LearnedView {
    std::string object;    // profile path ("Camera.@SBZClampCameraRotationModifier")
    std::string property;  // reflected rotator ("" = raw offsets)
    std::vector<int32_t> offsets;
    int32_t rotator_size = 24;
    bool world_rotation = false;
    uint32_t pitch_view = 0;  // PitchView=: its live test moved yaw only, pitch goes to that view
    std::string label;  // tab label in the settings panel
    std::string note;   // a comment written above the section (where and when it was learned)
};

// The view already turns that rotation (same object path, property or raw offsets).
bool same_rotation(const View& view, const LearnedView& learned);

enum class AddResult { Added, Duplicate, Failed };

// Adds `learned` to the profile at `path` (created if missing) as [View.Auto<id>] with Match=Camera and Parent=1, and
// appends it to Views= (in [Gyro4All]). The id is the lowest free one from 2, skipping `reserved` (the core's
// sensitivity profiles). Everything else in the file is kept. Duplicate when a view in it already turns that rotation.
AddResult add_view(const std::wstring& path, const std::string& exe_stem, const LearnedView& learned,
                   const std::vector<uint32_t>& reserved, uint32_t& id, std::string& key);

}  // namespace gyro4all::unreal
