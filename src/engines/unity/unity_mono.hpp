#pragma once
// The Unity module's access to the game's managed side through the embedded Mono runtime's own API (exports of
// MonoBleedingEdge\EmbedRuntime\mono-2.0-bdwgc.dll): classes and members found by name (reflection), fields read,
// property getters and Unity's Transform/Camera/Time/Cursor called with mono_runtime_invoke. Game thread only (Unity's
// main thread, where the scripts run and which Mono already knows); nothing is kept across frames but classes and
// members (Mono never unloads a player's assemblies), so a destroyed object is never touched later.
#include "unity_path.hpp"

#include <string>
#include <vector>

namespace gyro4all::unity::mono {

struct Object;  // MonoObject
struct Class;   // MonoClass
struct Field;   // MonoClassField
struct Method;  // MonoMethod

enum class Runtime {
    NotLoaded,    // the runtime or UnityEngine's assembly isn't loaded yet (asked again later)
    Ready,
    Unsupported,  // no Mono (IL2CPP build) or an API export is missing: never
};
// Game thread: binds the runtime's API and Unity's core classes once they're loaded; detail says why not.
Runtime bind(std::string& detail);

// A value at the end of a path: an object (a live one: a destroyed Unity object counts as none) or a number (bool,
// integers, float, double).
struct Value {
    Object* object = nullptr;
    double number = 0;
    bool is_number = false;
    bool found() const { return object || is_number; }
};

// A profile path with its members looked up by class (cached per step). Game thread.
class Path {
public:
    Path() = default;
    explicit Path(MemberPath path) : path_(std::move(path)), steps_(path_.members.size()) {}
    const MemberPath& spec() const { return path_; }
    bool empty() const { return path_.members.empty() && path_.assembly.empty() && !path_.constant; }
    // Follows the path from `from` (paths from the player) or from its static root. False when a link is null or
    // destroyed (why_not: which one), or a member doesn't exist (logged once).
    bool resolve(Object* from, Value& out);
    const std::string& why_not() const { return why_not_; }

private:
    struct Step {
        Class* klass = nullptr;  // the class the member was looked up on
        Field* field = nullptr;
        Method* getter = nullptr;
        int type = 0;            // MONO_TYPE_*
        bool reference = false;
        bool is_static = false;
        bool missing = false;    // not found on klass (logged)
    };
    bool lookup(Class* klass, size_t index);
    bool read(Object* obj, size_t index, Object*& next, double& number, bool& is_number);
    MemberPath path_;
    std::vector<Step> steps_;
    Class* root_ = nullptr;
    bool root_missing_ = false;
    std::string why_not_;
};

// Unity helpers. A Transform itself, else a Component's or GameObject's transform (nullptr otherwise).
Object* transform_of(Object* obj);
bool euler_angles(Object* transform, bool local, float (&xyz)[3]);
bool set_euler_angles(Object* transform, bool local, const float (&xyz)[3]);
bool time_scale(float& scale);
bool cursor_free(bool& free);  // a menu's mouse cursor: Cursor.lockState not Locked (and Cursor.visible, if kept)
bool camera_fov(Object* camera, float& vertical_degrees);  // the object must be a UnityEngine.Camera
std::string class_name(Object* obj);

}  // namespace gyro4all::unity::mono
