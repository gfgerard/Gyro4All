// The Unity module's Mono access (unity_mono.hpp). The API is Mono's embedding API, exported by every Unity Mono
// runtime (mono-2.0-bdwgc.dll since Unity 2017, mono.dll before): the same calls Unity itself makes to run scripts.
#include "unity_mono.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstring>
#include <format>
#include <map>

namespace gyro4all::unity::mono {

namespace {

struct Domain;
struct Assembly;
struct Image;
struct Property;
struct Type;
struct VTable;
struct Signature;

// MonoTypeEnum values used here.
enum : int {
    kBoolean = 0x02, kChar = 0x03, kI1 = 0x04, kU1 = 0x05, kI2 = 0x06, kU2 = 0x07, kI4 = 0x08, kU4 = 0x09,
    kI8 = 0x0a, kU8 = 0x0b, kR4 = 0x0c, kR8 = 0x0d,
};
constexpr uint32_t kStaticFlag = 0x10;  // FIELD_ATTRIBUTE_STATIC, METHOD_ATTRIBUTE_STATIC

struct Api {
    Domain* (*get_root_domain)();
    Domain* (*domain_get)();
    void* (*thread_attach)(Domain*);
    void (*assembly_foreach)(void (*)(void*, void*), void*);
    Image* (*assembly_get_image)(Assembly*);
    const char* (*image_get_name)(Image*);
    Class* (*class_from_name)(Image*, const char*, const char*);
    Field* (*class_get_field_from_name)(Class*, const char*);
    Property* (*class_get_property_from_name)(Class*, const char*);
    Method* (*class_get_method_from_name)(Class*, const char*, int);
    const char* (*class_get_name)(Class*);
    const char* (*class_get_namespace)(Class*);
    VTable* (*class_vtable)(Domain*, Class*);
    Method* (*property_get_get_method)(Property*);
    Type* (*field_get_type)(Field*);
    uint32_t (*field_get_flags)(Field*);
    void (*field_get_value)(Object*, Field*, void*);
    void (*field_static_get_value)(VTable*, Field*, void*);
    int (*type_get_type)(Type*);
    int (*type_is_reference)(Type*);
    Signature* (*method_signature)(Method*);
    Type* (*signature_get_return_type)(Signature*);
    uint32_t (*method_get_flags)(Method*, uint32_t*);
    Object* (*runtime_invoke)(Method*, void*, void**, Object**);
    void* (*object_unbox)(Object*);
    Class* (*object_get_class)(Object*);
    Object* (*object_isinst)(Object*, Class*);
};
Api g_api{};
bool g_bound = false;
Domain* g_domain = nullptr;

// Unity's core classes and the members the module calls.
struct Unity {
    Class *object, *transform, *component, *game_object, *camera, *time, *cursor;
    Field* cached_ptr;  // UnityEngine.Object.m_CachedPtr: 0 once the native object is destroyed
    Method *euler_get, *euler_set, *local_euler_get, *local_euler_set, *component_transform, *object_transform,
        *fov_get, *time_scale_get, *lock_state_get, *visible_get;
} g_unity{};

std::map<std::string, Image*> g_images;
int g_exception_logs = 0;

template <class F>
bool bind_export(HMODULE m, const char* name, F& fn, std::string& missing) {
    fn = reinterpret_cast<F>(GetProcAddress(m, name));
    if (!fn) missing += missing.empty() ? name : std::string(", ") + name;
    return fn != nullptr;
}

bool bind_api(HMODULE m, std::string& missing) {
    Api& a = g_api;
    bind_export(m, "mono_get_root_domain", a.get_root_domain, missing);
    bind_export(m, "mono_domain_get", a.domain_get, missing);
    bind_export(m, "mono_thread_attach", a.thread_attach, missing);
    bind_export(m, "mono_assembly_foreach", a.assembly_foreach, missing);
    bind_export(m, "mono_assembly_get_image", a.assembly_get_image, missing);
    bind_export(m, "mono_image_get_name", a.image_get_name, missing);
    bind_export(m, "mono_class_from_name", a.class_from_name, missing);
    bind_export(m, "mono_class_get_field_from_name", a.class_get_field_from_name, missing);
    bind_export(m, "mono_class_get_property_from_name", a.class_get_property_from_name, missing);
    bind_export(m, "mono_class_get_method_from_name", a.class_get_method_from_name, missing);
    bind_export(m, "mono_class_get_name", a.class_get_name, missing);
    bind_export(m, "mono_class_get_namespace", a.class_get_namespace, missing);
    bind_export(m, "mono_class_vtable", a.class_vtable, missing);
    bind_export(m, "mono_property_get_get_method", a.property_get_get_method, missing);
    bind_export(m, "mono_field_get_type", a.field_get_type, missing);
    bind_export(m, "mono_field_get_flags", a.field_get_flags, missing);
    bind_export(m, "mono_field_get_value", a.field_get_value, missing);
    bind_export(m, "mono_field_static_get_value", a.field_static_get_value, missing);
    bind_export(m, "mono_type_get_type", a.type_get_type, missing);
    bind_export(m, "mono_type_is_reference", a.type_is_reference, missing);
    bind_export(m, "mono_method_signature", a.method_signature, missing);
    bind_export(m, "mono_signature_get_return_type", a.signature_get_return_type, missing);
    bind_export(m, "mono_method_get_flags", a.method_get_flags, missing);
    bind_export(m, "mono_runtime_invoke", a.runtime_invoke, missing);
    bind_export(m, "mono_object_unbox", a.object_unbox, missing);
    bind_export(m, "mono_object_get_class", a.object_get_class, missing);
    bind_export(m, "mono_object_isinst", a.object_isinst, missing);
    return missing.empty();
}

// A loaded assembly's image by assembly name (cached once found; a player's assemblies are never unloaded).
Image* image(const std::string& name) {
    if (auto it = g_images.find(name); it != g_images.end()) return it->second;
    struct Ctx {
        const std::string* name;
        Image* found;
    } ctx{&name, nullptr};
    g_api.assembly_foreach(
        [](void* assembly, void* user) {
            auto* c = static_cast<Ctx*>(user);
            if (c->found) return;
            Image* img = g_api.assembly_get_image(static_cast<Assembly*>(assembly));
            const char* n = img ? g_api.image_get_name(img) : nullptr;
            if (n && _stricmp(n, c->name->c_str()) == 0) c->found = img;
        },
        &ctx);
    if (ctx.found) g_images[name] = ctx.found;
    return ctx.found;
}

// mono_runtime_invoke with the managed exception caught (logged, a few times).
Object* invoke(Method* m, void* obj, void** params, bool& ok, const char* what) {
    Object* exc = nullptr;
    Object* result = g_api.runtime_invoke(m, obj, params, &exc);
    ok = exc == nullptr;
    if (!ok && g_exception_logs < 20) {
        ++g_exception_logs;
        log("unity: %s threw %s%s", what, class_name(exc).c_str(),
            g_exception_logs == 20 ? " (no more of these logged)" : "");
    }
    return ok ? result : nullptr;
}

bool to_number(int type, const void* data, double& out) {
    switch (type) {
    case kBoolean: out = *static_cast<const uint8_t*>(data) ? 1 : 0; return true;
    case kChar: case kU2: out = *static_cast<const uint16_t*>(data); return true;
    case kI1: out = *static_cast<const int8_t*>(data); return true;
    case kU1: out = *static_cast<const uint8_t*>(data); return true;
    case kI2: out = *static_cast<const int16_t*>(data); return true;
    case kI4: out = *static_cast<const int32_t*>(data); return true;
    case kU4: out = *static_cast<const uint32_t*>(data); return true;
    case kI8: out = double(*static_cast<const int64_t*>(data)); return true;
    case kU8: out = double(*static_cast<const uint64_t*>(data)); return true;
    case kR4: out = *static_cast<const float*>(data); return true;
    case kR8: out = *static_cast<const double*>(data); return true;
    default: return false;
    }
}

// A live object: not null, and for Unity objects, its native object not destroyed (Unity's "== null").
bool alive(Object* obj) {
    if (!obj) return false;
    if (!g_api.object_isinst(obj, g_unity.object)) return true;
    void* native = nullptr;
    g_api.field_get_value(obj, g_unity.cached_ptr, &native);
    return native != nullptr;
}

Method* getter(Class* klass, const char* property) {
    Property* p = g_api.class_get_property_from_name(klass, property);
    return p ? g_api.property_get_get_method(p) : nullptr;
}

Method* setter(Class* klass, const char* name) { return g_api.class_get_method_from_name(klass, name, 1); }

bool bind_unity(std::string& detail) {
    // UnityEngine.CoreModule since Unity 2017.2; one UnityEngine assembly before.
    Image* core = image("UnityEngine.CoreModule");
    if (!core) core = image("UnityEngine");
    if (!core) {
        detail = "UnityEngine.CoreModule not loaded yet";
        return false;
    }
    Unity& u = g_unity;
    u.object = g_api.class_from_name(core, "UnityEngine", "Object");
    u.transform = g_api.class_from_name(core, "UnityEngine", "Transform");
    u.component = g_api.class_from_name(core, "UnityEngine", "Component");
    u.game_object = g_api.class_from_name(core, "UnityEngine", "GameObject");
    u.camera = g_api.class_from_name(core, "UnityEngine", "Camera");
    u.time = g_api.class_from_name(core, "UnityEngine", "Time");
    u.cursor = g_api.class_from_name(core, "UnityEngine", "Cursor");
    if (!u.object || !u.transform || !u.component || !u.game_object || !u.camera || !u.time || !u.cursor) {
        detail = "UnityEngine's Object/Transform/Component/GameObject/Camera/Time/Cursor classes not all found";
        return false;
    }
    u.cached_ptr = g_api.class_get_field_from_name(u.object, "m_CachedPtr");
    u.euler_get = getter(u.transform, "eulerAngles");
    u.euler_set = setter(u.transform, "set_eulerAngles");
    u.local_euler_get = getter(u.transform, "localEulerAngles");
    u.local_euler_set = setter(u.transform, "set_localEulerAngles");
    u.component_transform = getter(u.component, "transform");
    u.object_transform = getter(u.game_object, "transform");
    u.fov_get = getter(u.camera, "fieldOfView");
    u.time_scale_get = getter(u.time, "timeScale");
    u.lock_state_get = getter(u.cursor, "lockState");
    u.visible_get = getter(u.cursor, "visible");
    // Builds strip managed code the game never uses (MOUSE's Cursor.visible has no getter: MODLOG 2026-11-11), so only
    // what turns the camera is required; without one of the others its signal is skipped (logged here).
    const std::pair<const void*, const char*> required[] = {
        {u.cached_ptr, "Object.m_CachedPtr"},           {u.euler_get, "Transform.eulerAngles"},
        {u.euler_set, "Transform.set_eulerAngles"},     {u.local_euler_get, "Transform.localEulerAngles"},
        {u.local_euler_set, "Transform.set_localEulerAngles"}, {u.component_transform, "Component.transform"}};
    const std::pair<const void*, const char*> optional[] = {
        {u.object_transform, "GameObject.transform (paths ending at a GameObject)"},
        {u.fov_get, "Camera.fieldOfView getter (zoom compensation)"},
        {u.time_scale_get, "Time.timeScale getter (pause)"},
        {u.lock_state_get, "Cursor.lockState getter (menus' cursor)"},
        {u.visible_get, "Cursor.visible getter (menus' cursor: lockState alone)"}};
    std::string missing;
    for (const auto& [found, name] : required)
        if (!found) missing += (missing.empty() ? "" : ", ") + std::string(name);
    if (!missing.empty()) {
        detail = "UnityEngine members not found: " + missing;
        return false;
    }
    for (const auto& [found, name] : optional)
        if (!found) log("unity: %s not in this build (stripped); skipped", name);
    return true;
}

}  // namespace

Runtime bind(std::string& detail) {
    static Runtime state = Runtime::NotLoaded;
    if (state != Runtime::NotLoaded) return state;
    if (!g_bound) {
        HMODULE m = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!m) m = GetModuleHandleW(L"mono-2.0-sgen.dll");
        if (!m) m = GetModuleHandleW(L"mono.dll");
        if (!m) {
            if (GetModuleHandleW(L"GameAssembly.dll")) {
                detail = "IL2CPP build (GameAssembly.dll, no Mono runtime): not supported yet";
                return state = Runtime::Unsupported;
            }
            detail = "Mono runtime not loaded yet";
            return state;
        }
        std::string missing;
        if (!bind_api(m, missing)) {
            detail = "the Mono runtime lacks " + missing;
            return state = Runtime::Unsupported;
        }
        g_bound = true;
    }
    if (!g_api.get_root_domain()) {
        detail = "Mono runtime not started yet";
        return state;
    }
    // Unity's main thread runs the scripts, so it is already attached; attach it otherwise (harmless if it is).
    g_domain = g_api.domain_get();
    if (!g_domain) {
        g_api.thread_attach(g_api.get_root_domain());
        g_domain = g_api.domain_get();
    }
    if (!g_domain) {
        detail = "can't attach the game thread to the Mono domain";
        return state;
    }
    if (!bind_unity(detail)) return state;
    detail.clear();
    return state = Runtime::Ready;
}

bool Path::lookup(Class* klass, size_t index) {
    Step& s = steps_[index];
    s = Step{};
    s.klass = klass;
    const std::string& name = path_.members[index];
    if (Field* f = g_api.class_get_field_from_name(klass, name.c_str())) {
        s.field = f;
        Type* t = g_api.field_get_type(f);
        s.type = g_api.type_get_type(t);
        s.reference = g_api.type_is_reference(t) != 0;
        s.is_static = (g_api.field_get_flags(f) & kStaticFlag) != 0;
    } else if (Method* get = getter(klass, name.c_str())) {
        s.getter = get;
        Type* t = g_api.signature_get_return_type(g_api.method_signature(get));
        s.type = g_api.type_get_type(t);
        s.reference = g_api.type_is_reference(t) != 0;
        s.is_static = (g_api.method_get_flags(get, nullptr) & kStaticFlag) != 0;
    } else {
        s.missing = true;
        log("unity: profile path %s: no field or property '%s' on %s.%s", describe(path_).c_str(), name.c_str(),
            g_api.class_get_namespace(klass), g_api.class_get_name(klass));
        return false;
    }
    if (!s.reference) {
        double probe = 0;
        const uint64_t zero = 0;
        if (!to_number(s.type, &zero, probe)) {
            s.missing = true;
            log("unity: profile path %s: '%s' is a value type this module doesn't read (type 0x%x)", describe(path_).c_str(),
                name.c_str(), s.type);
            return false;
        }
    }
    return true;
}

bool Path::read(Object* obj, size_t index, Object*& next, double& number, bool& is_number) {
    const Step& s = steps_[index];
    next = nullptr;
    is_number = false;
    if (s.field) {
        uint64_t buf[2] = {};
        if (s.is_static) {
            VTable* vt = g_api.class_vtable(g_domain, s.klass);
            if (!vt) return false;
            g_api.field_static_get_value(vt, s.field, buf);
        } else {
            g_api.field_get_value(obj, s.field, buf);
        }
        if (s.reference) next = *reinterpret_cast<Object**>(buf);
        else is_number = to_number(s.type, buf, number);
        return true;
    }
    bool ok = false;
    Object* r = invoke(s.getter, s.is_static ? nullptr : obj, nullptr, ok, "a profile path's property getter");
    if (!ok) return false;
    if (s.reference) next = r;
    else if (r) is_number = to_number(s.type, g_api.object_unbox(r), number);
    return true;
}

bool Path::resolve(Object* from, Value& out) {
    out = {};
    if (path_.constant) {
        out.number = *path_.constant;
        out.is_number = true;
        return true;
    }
    Object* cur = from;
    size_t i = 0;
    if (!path_.assembly.empty()) {
        if (root_missing_) return false;
        if (!root_) {
            Image* img = image(path_.assembly);
            if (!img) {
                why_not_ = "assembly " + path_.assembly + " not loaded";
                return false;
            }
            root_ = g_api.class_from_name(img, path_.ns.c_str(), path_.cls.c_str());
            if (!root_) {
                root_missing_ = true;
                log("unity: profile path %s: no class %s%s%s in %s", describe(path_).c_str(), path_.ns.c_str(),
                    path_.ns.empty() ? "" : ".", path_.cls.c_str(), path_.assembly.c_str());
                return false;
            }
        }
        Step& s = steps_[0];
        if (s.missing) return false;
        if (!s.klass && !lookup(root_, 0)) return false;
        if (!s.is_static) {
            if (!s.missing)
                log("unity: profile path %s: '%s' isn't static", describe(path_).c_str(), path_.members[0].c_str());
            s.missing = true;
            return false;
        }
        double number = 0;
        bool is_number = false;
        if (!read(nullptr, 0, cur, number, is_number)) return false;
        if (is_number) {
            if (path_.members.size() > 1) {
                why_not_ = path_.members[0] + " is a number";
                return false;
            }
            out.number = number;
            out.is_number = true;
            return true;
        }
        i = 1;
    }
    for (; i < path_.members.size(); ++i) {
        if (!alive(cur)) {
            why_not_ = i ? path_.members[i - 1] + " is null" : "no object";
            return false;
        }
        Class* klass = g_api.object_get_class(cur);
        Step& s = steps_[i];
        if (s.klass != klass) {
            if (!lookup(klass, i)) return false;
        } else if (s.missing) {
            return false;
        }
        double number = 0;
        bool is_number = false;
        Object* next = nullptr;
        if (!read(cur, i, next, number, is_number)) {
            why_not_ = path_.members[i] + " can't be read";
            return false;
        }
        if (is_number) {
            if (i + 1 < path_.members.size()) {
                why_not_ = path_.members[i] + " is a number";
                return false;
            }
            out.number = number;
            out.is_number = true;
            return true;
        }
        cur = next;
    }
    if (!alive(cur)) {
        why_not_ = path_.members.empty() ? "no object" : path_.members.back() + " is null";
        return false;
    }
    out.object = cur;
    why_not_.clear();
    return true;
}

Object* transform_of(Object* obj) {
    if (!alive(obj)) return nullptr;
    if (g_api.object_isinst(obj, g_unity.transform)) return obj;
    Method* get = g_api.object_isinst(obj, g_unity.component) ? g_unity.component_transform
                  : g_api.object_isinst(obj, g_unity.game_object) ? g_unity.object_transform
                                                                  : nullptr;
    if (!get) return nullptr;
    bool ok = false;
    Object* t = invoke(get, obj, nullptr, ok, "transform");
    return ok && alive(t) ? t : nullptr;
}

bool euler_angles(Object* transform, bool local, float (&xyz)[3]) {
    bool ok = false;
    Object* r = invoke(local ? g_unity.local_euler_get : g_unity.euler_get, transform, nullptr, ok,
                       local ? "Transform.localEulerAngles" : "Transform.eulerAngles");
    if (!ok || !r) return false;
    std::memcpy(xyz, g_api.object_unbox(r), sizeof xyz);
    return true;
}

bool set_euler_angles(Object* transform, bool local, const float (&xyz)[3]) {
    float v[3] = {xyz[0], xyz[1], xyz[2]};
    void* params[] = {v};
    bool ok = false;
    invoke(local ? g_unity.local_euler_set : g_unity.euler_set, transform, params, ok,
           local ? "Transform.localEulerAngles=" : "Transform.eulerAngles=");
    return ok;
}

bool time_scale(float& scale) {
    if (!g_unity.time_scale_get) return false;
    bool ok = false;
    Object* r = invoke(g_unity.time_scale_get, nullptr, nullptr, ok, "Time.timeScale");
    if (!ok || !r) return false;
    scale = *static_cast<float*>(g_api.object_unbox(r));
    return true;
}

bool cursor_free(bool& free) {
    if (!g_unity.lock_state_get) return false;
    bool ok = false;
    Object* lock = invoke(g_unity.lock_state_get, nullptr, nullptr, ok, "Cursor.lockState");
    if (!ok || !lock) return false;
    // CursorLockMode: None 0, Locked 1, Confined 2. Visible too when the build can tell.
    free = *static_cast<int32_t*>(g_api.object_unbox(lock)) != 1;
    if (free && g_unity.visible_get) {
        Object* visible = invoke(g_unity.visible_get, nullptr, nullptr, ok, "Cursor.visible");
        if (!ok || !visible) return false;
        free = *static_cast<uint8_t*>(g_api.object_unbox(visible)) != 0;
    }
    return true;
}

bool camera_fov(Object* camera, float& vertical_degrees) {
    if (!g_unity.fov_get || !alive(camera) || !g_api.object_isinst(camera, g_unity.camera)) return false;
    bool ok = false;
    Object* r = invoke(g_unity.fov_get, camera, nullptr, ok, "Camera.fieldOfView");
    if (!ok || !r) return false;
    vertical_degrees = *static_cast<float*>(g_api.object_unbox(r));
    return true;
}

std::string class_name(Object* obj) {
    if (!obj || !g_bound) return "null";
    Class* k = g_api.object_get_class(obj);
    const char* ns = g_api.class_get_namespace(k);
    return ns && *ns ? std::format("{}.{}", ns, g_api.class_get_name(k)) : std::string(g_api.class_get_name(k));
}

}  // namespace gyro4all::unity::mono
