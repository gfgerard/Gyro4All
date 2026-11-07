#include "unity_runtime.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <cstring>

namespace gyro4all::unity {
namespace {

template <typename T>
bool load_export(HMODULE module, const char* name, T& function) {
    function = reinterpret_cast<T>(GetProcAddress(module, name));
    return function != nullptr;
}

struct MonoClassSearch {
    Runtime* runtime;
    const char* name_space;
    const char* name;
    void* found = nullptr;
};

}  // namespace

const char* backend_name(Backend backend) {
    switch (backend) {
        case Backend::Mono: return "Mono";
        case Backend::Il2Cpp: return "IL2CPP";
        case Backend::Failed: return "failed";
        default: return "waiting";
    }
}

Backend Runtime::missing(const char* name) {
    backend_ = Backend::Failed;
    log("unity: runtime export missing: %s", name);
    return backend_;
}

Backend Runtime::initialize_mono(HMODULE module) {
#define MONO_EXPORT(name)                                                                                            \
    if (!load_export(module, #name, name##_)) return missing(#name)
    MONO_EXPORT(mono_get_root_domain);
    MONO_EXPORT(mono_thread_attach);
    MONO_EXPORT(mono_assembly_foreach);
    MONO_EXPORT(mono_assembly_get_image);
    MONO_EXPORT(mono_class_from_name);
    MONO_EXPORT(mono_class_get_method_from_name);
    MONO_EXPORT(mono_class_get_parent);
    MONO_EXPORT(mono_runtime_invoke);
    MONO_EXPORT(mono_object_unbox);
    MONO_EXPORT(mono_object_get_class);
    MONO_EXPORT(mono_class_get_type);
    MONO_EXPORT(mono_type_get_object);
    MONO_EXPORT(mono_array_addr_with_size);
    MONO_EXPORT(mono_class_get_field_from_name);
#undef MONO_EXPORT
    // Unity 5's mono.dll (The Forest's) doesn't export mono_array_length; Runtime::array_length reads the field then.
    load_export(module, "mono_array_length", mono_array_length_);
    load_export(module, "mono_lookup_internal_call", mono_lookup_internal_call_);
    load_export(module, "mono_method_get_name", mono_method_get_name_);
    load_export(module, "mono_class_get_element_class", mono_class_get_element_class_);
    load_export(module, "mono_class_array_element_size", mono_class_array_element_size_);
    load_export(module, "mono_class_is_valuetype", mono_class_is_valuetype_);
    load_export(module, "mono_gchandle_new", mono_gchandle_new_);
    load_export(module, "mono_gchandle_new_weakref", mono_gchandle_new_weakref_);
    load_export(module, "mono_gchandle_get_target", mono_gchandle_get_target_);
    load_export(module, "mono_gchandle_free", mono_gchandle_free_);
    mono_handles_v2_ = load_export(module, "mono_gchandle_new_v2", mono_gchandle_new_v2_) &&
                       load_export(module, "mono_gchandle_new_weakref_v2", mono_gchandle_new_weakref_v2_) &&
                       load_export(module, "mono_gchandle_get_target_v2", mono_gchandle_get_target_v2_) &&
                       load_export(module, "mono_gchandle_free_v2", mono_gchandle_free_v2_);
    reflection_ = load_export(module, "mono_class_get_fields", mono_class_get_fields_) &&
                  load_export(module, "mono_field_get_name", mono_field_get_name_) &&
                  load_export(module, "mono_field_get_offset", mono_field_get_offset_) &&
                  load_export(module, "mono_field_get_flags", mono_field_get_flags_) &&
                  load_export(module, "mono_field_get_type", mono_field_get_type_) &&
                  load_export(module, "mono_type_get_type", mono_type_get_type_) &&
                  load_export(module, "mono_class_from_mono_type", mono_class_from_mono_type_) &&
                  load_export(module, "mono_class_get_name", mono_class_get_name_) &&
                  load_export(module, "mono_class_get_namespace", mono_class_get_namespace_);
    backend_ = Backend::Mono;
    return backend_;
}

Backend Runtime::initialize_il2cpp(HMODULE module) {
#define IL2CPP_EXPORT(name)                                                                                          \
    if (!load_export(module, #name, name##_)) return missing(#name)
    IL2CPP_EXPORT(il2cpp_domain_get);
    IL2CPP_EXPORT(il2cpp_thread_attach);
    IL2CPP_EXPORT(il2cpp_domain_get_assemblies);
    IL2CPP_EXPORT(il2cpp_assembly_get_image);
    IL2CPP_EXPORT(il2cpp_class_from_name);
    IL2CPP_EXPORT(il2cpp_class_get_method_from_name);
    IL2CPP_EXPORT(il2cpp_class_get_parent);
    IL2CPP_EXPORT(il2cpp_runtime_invoke);
    IL2CPP_EXPORT(il2cpp_object_unbox);
    IL2CPP_EXPORT(il2cpp_object_get_class);
    IL2CPP_EXPORT(il2cpp_class_get_type);
    IL2CPP_EXPORT(il2cpp_type_get_object);
    IL2CPP_EXPORT(il2cpp_array_length);
    IL2CPP_EXPORT(il2cpp_class_get_field_from_name);
#undef IL2CPP_EXPORT
    // Some IL2CPP builds (Viewfinder's and Deliver At All Costs' GameAssembly.dll) don't export
    // il2cpp_array_addr_with_size; Runtime::array_element falls back to the x64 array layout then.
    static bool logged = false;
    if (!load_export(module, "il2cpp_array_addr_with_size", il2cpp_array_addr_with_size_) && !logged) {
        logged = true;
        log("unity: il2cpp_array_addr_with_size missing; using the x64 array layout (elements at offset 32)");
    }
    load_export(module, "il2cpp_method_get_name", il2cpp_method_get_name_);
    load_export(module, "il2cpp_class_get_element_class", il2cpp_class_get_element_class_);
    load_export(module, "il2cpp_class_array_element_size", il2cpp_class_array_element_size_);
    load_export(module, "il2cpp_class_is_valuetype", il2cpp_class_is_valuetype_);
    load_export(module, "il2cpp_gchandle_new", il2cpp_gchandle_new_);
    load_export(module, "il2cpp_gchandle_new_weakref", il2cpp_gchandle_new_weakref_);
    load_export(module, "il2cpp_gchandle_get_target", il2cpp_gchandle_get_target_);
    load_export(module, "il2cpp_gchandle_free", il2cpp_gchandle_free_);
    reflection_ = load_export(module, "il2cpp_class_get_fields", il2cpp_class_get_fields_) &&
                  load_export(module, "il2cpp_field_get_name", il2cpp_field_get_name_) &&
                  load_export(module, "il2cpp_field_get_offset", il2cpp_field_get_offset_) &&
                  load_export(module, "il2cpp_field_get_flags", il2cpp_field_get_flags_) &&
                  load_export(module, "il2cpp_field_get_type", il2cpp_field_get_type_) &&
                  load_export(module, "il2cpp_type_get_type", il2cpp_type_get_type_) &&
                  load_export(module, "il2cpp_class_from_type", il2cpp_class_from_type_) &&
                  load_export(module, "il2cpp_class_get_name", il2cpp_class_get_name_) &&
                  load_export(module, "il2cpp_class_get_namespace", il2cpp_class_get_namespace_);
    backend_ = Backend::Il2Cpp;
    return backend_;
}

Backend Runtime::initialize() {
    if (backend_ != Backend::Waiting) return backend_;
    if (HMODULE module = GetModuleHandleW(L"GameAssembly.dll")) initialize_il2cpp(module);
    if (backend_ == Backend::Waiting) {
        const wchar_t* names[] = {L"mono-2.0-bdwgc.dll", L"mono-2.0-sgen.dll", L"mono.dll"};
        for (const wchar_t* name : names) {
            if (HMODULE module = GetModuleHandleW(name)) {
                initialize_mono(module);
                break;
            }
        }
    }
    if (backend_ == Backend::Waiting || backend_ == Backend::Failed) return backend_;
    domain_ = backend_ == Backend::Mono ? mono_get_root_domain_() : il2cpp_domain_get_();
    if (!domain_) {
        backend_ = Backend::Waiting;
        return backend_;
    }
    if (!attached_) {
        if (backend_ == Backend::Mono ? mono_thread_attach_(domain_) == nullptr : il2cpp_thread_attach_(domain_) == nullptr) {
            backend_ = Backend::Failed;
            log("unity: couldn't attach the game thread to the scripting domain");
            return backend_;
        }
        attached_ = true;
    }
    log("unity: %s runtime attached (reflection exports %s)", backend_name(backend_), reflection_ ? "found" : "missing");
    return backend_;
}

void* Runtime::find_mono_class(const char* name_space, const char* name) {
    MonoClassSearch search{this, name_space, name};
    mono_assembly_foreach_(
        [](void* assembly, void* data) {
            auto& search = *static_cast<MonoClassSearch*>(data);
            if (search.found) return;
            void* image = search.runtime->mono_assembly_get_image_(assembly);
            if (image) search.found = search.runtime->mono_class_from_name_(image, search.name_space, search.name);
        },
        &search);
    return search.found;
}

void* Runtime::find_il2cpp_class(const char* name_space, const char* name) {
    size_t count = 0;
    void** assemblies = il2cpp_domain_get_assemblies_(domain_, &count);
    if (!assemblies || count > 4096) return nullptr;
    for (size_t i = 0; i < count; ++i) {
        void* image = il2cpp_assembly_get_image_(assemblies[i]);
        if (void* klass = image ? il2cpp_class_from_name_(image, name_space, name) : nullptr) return klass;
    }
    return nullptr;
}

void* Runtime::find_class(const char* name_space, const char* name) {
    if (!ready()) return nullptr;
    return backend_ == Backend::Mono ? find_mono_class(name_space, name) : find_il2cpp_class(name_space, name);
}

void* Runtime::find_method(void* klass, const char* name, int parameter_count) {
    for (int depth = 0; klass && depth < 32; ++depth) {
        if (void* method = backend_ == Backend::Mono ? mono_class_get_method_from_name_(klass, name, parameter_count)
                                                     : il2cpp_class_get_method_from_name_(klass, name, parameter_count))
            return method;
        klass = class_parent(klass);
    }
    return nullptr;
}

void* Runtime::invoke(void* method, void* object, void** arguments) {
    threw_ = false;
    if (!method) return nullptr;
    void* exception = nullptr;
    void* result = backend_ == Backend::Mono ? mono_runtime_invoke_(method, object, arguments, &exception)
                                             : il2cpp_runtime_invoke_(method, object, arguments, &exception);
    if (exception) {
        threw_ = true;
        static int logged = 0;
        if (logged < 20) {
            ++logged;
            const char* name = backend_ == Backend::Mono ? (mono_method_get_name_ ? mono_method_get_name_(method) : nullptr)
                                                         : (il2cpp_method_get_name_ ? il2cpp_method_get_name_(method) : nullptr);
            log("unity: the managed call %s threw an exception%s", name ? name : "(unknown)",
                logged == 20 ? " (no more of these are logged)" : "");
        }
        return nullptr;
    }
    return result;
}

void* Runtime::unbox(void* object) const {
    if (!object) return nullptr;
    return backend_ == Backend::Mono ? mono_object_unbox_(object) : il2cpp_object_unbox_(object);
}

void* Runtime::object_class(void* object) const {
    if (!object) return nullptr;
    return backend_ == Backend::Mono ? mono_object_get_class_(object) : il2cpp_object_get_class_(object);
}

void* Runtime::find_objects_of_type(void* klass) {
    if (!klass) return nullptr;
    void* object_class = find_class("UnityEngine", "Object");
    void* find = find_method(object_class, "FindObjectsOfType", 1);
    void* type = backend_ == Backend::Mono ? mono_type_get_object_(domain_, mono_class_get_type_(klass))
                                           : il2cpp_type_get_object_(il2cpp_class_get_type_(klass));
    void* arguments[] = {type};
    return invoke(find, nullptr, arguments);
}

size_t Runtime::array_length(void* array) {
    if (!array) return 0;
    size_t length = 0;
    if (backend_ != Backend::Mono) {
        length = il2cpp_array_length_(array);
    } else if (mono_array_length_) {
        length = mono_array_length_(array);
    } else {
        // MonoArray x64 layout: MonoObject (16 bytes: vtable, synchronisation), bounds (8), max_length (read as 32 bits:
        // it is 32 or 64 wide by build), then the elements at 32, where mono_array_addr_with_size puts them.
        uint32_t stored = 0;
        if (!mem::rd(reinterpret_cast<uintptr_t>(array) + 24, stored)) return 0;
        length = stored;
    }
    return length <= 65536 ? length : 0;
}

void* Runtime::array_element(void* array, size_t index, size_t element_size) {
    if (!array || index >= array_length(array)) return nullptr;
    if (backend_ == Backend::Mono) return mono_array_addr_with_size_(array, int(element_size), index);
    if (il2cpp_array_addr_with_size_) return il2cpp_array_addr_with_size_(array, int(element_size), index);
    // Il2CppArray x64 layout: Il2CppObject (16 bytes: klass, monitor), bounds (8), max_length (8), then the elements. The
    // length is checked where the layout says it is: a layout that differs finds nothing, rather than wrong objects.
    size_t stored = 0;
    if (!mem::rd(reinterpret_cast<uintptr_t>(array) + 24, stored) || stored != array_length(array)) {
        static bool told = false;
        if (!told) log("unity: this IL2CPP build's array layout isn't the expected one: objects can't be listed");
        told = true;
        return nullptr;
    }
    return static_cast<char*>(array) + 32 + element_size * index;
}

void* Runtime::element_class(void* array_class) const {
    if (!array_class) return nullptr;
    if (backend_ == Backend::Mono) return mono_class_get_element_class_ ? mono_class_get_element_class_(array_class) : nullptr;
    return il2cpp_class_get_element_class_ ? il2cpp_class_get_element_class_(array_class) : nullptr;
}

size_t Runtime::element_size(void* element_class) const {
    if (!element_class) return 0;
    const int size = backend_ == Backend::Mono ? (mono_class_array_element_size_ ? mono_class_array_element_size_(element_class) : 0)
                                               : (il2cpp_class_array_element_size_ ? il2cpp_class_array_element_size_(element_class) : 0);
    return size > 0 ? size_t(size) : 0;
}

bool Runtime::alive(void* object) {
    if (!object) return false;
    if (!cached_ptr_looked_up_ && reflection_) {
        cached_ptr_looked_up_ = true;
        void* field = find_field(find_class("UnityEngine", "Object"), "m_CachedPtr");
        cached_ptr_offset_ = field ? field_offset(field) : 0;
        if (!cached_ptr_offset_) log("unity: UnityEngine.Object.m_CachedPtr not found: destroyed objects can't be told apart");
    }
    if (!cached_ptr_offset_) return true;
    uintptr_t native = 0;
    return mem::rd(reinterpret_cast<uintptr_t>(object) + cached_ptr_offset_, native) && native != 0;
}

void* Runtime::find_field(void* klass, const char* name) {
    if (!klass) return nullptr;
    return backend_ == Backend::Mono ? mono_class_get_field_from_name_(klass, name)
                                     : il2cpp_class_get_field_from_name_(klass, name);
}

void* Runtime::next_field(void* klass, void** iterator) {
    if (!reflection_ || !klass) return nullptr;
    return backend_ == Backend::Mono ? mono_class_get_fields_(klass, iterator) : il2cpp_class_get_fields_(klass, iterator);
}

const char* Runtime::field_name(void* field) const {
    if (!reflection_ || !field) return "";
    const char* name = backend_ == Backend::Mono ? mono_field_get_name_(field) : il2cpp_field_get_name_(field);
    return name ? name : "";
}

uint32_t Runtime::field_offset(void* field) const {
    if (!reflection_ || !field) return 0;
    return backend_ == Backend::Mono ? mono_field_get_offset_(field) : il2cpp_field_get_offset_(field);
}

bool Runtime::field_is_static(void* field) const {
    constexpr uint32_t kFieldStatic = 0x10;  // FieldAttributes.Static (literals have it too)
    if (!reflection_ || !field) return true;
    return ((backend_ == Backend::Mono ? mono_field_get_flags_(field) : uint32_t(il2cpp_field_get_flags_(field))) &
            kFieldStatic) != 0;
}

int Runtime::field_type(void* field) const {
    if (!reflection_ || !field) return 0;
    return backend_ == Backend::Mono ? mono_type_get_type_(mono_field_get_type_(field))
                                     : il2cpp_type_get_type_(il2cpp_field_get_type_(field));
}

void* Runtime::field_class(void* field) const {
    if (!reflection_ || !field) return nullptr;
    return backend_ == Backend::Mono ? mono_class_from_mono_type_(mono_field_get_type_(field))
                                     : il2cpp_class_from_type_(il2cpp_field_get_type_(field));
}

const char* Runtime::class_name(void* klass) const {
    if (!reflection_ || !klass) return "";
    const char* name = backend_ == Backend::Mono ? mono_class_get_name_(klass) : il2cpp_class_get_name_(klass);
    return name ? name : "";
}

const char* Runtime::class_namespace(void* klass) const {
    if (!reflection_ || !klass) return "";
    const char* name = backend_ == Backend::Mono ? mono_class_get_namespace_(klass) : il2cpp_class_get_namespace_(klass);
    return name ? name : "";
}

void* Runtime::class_parent(void* klass) const {
    if (!klass) return nullptr;
    return backend_ == Backend::Mono ? mono_class_get_parent_(klass) : il2cpp_class_get_parent_(klass);
}

bool Runtime::class_is_value_type(void* klass) const {
    if (!klass) return false;
    if (backend_ == Backend::Mono) return mono_class_is_valuetype_ && mono_class_is_valuetype_(klass) != 0;
    return il2cpp_class_is_valuetype_ && il2cpp_class_is_valuetype_(klass);
}

std::string Runtime::class_full_name(void* klass) const {
    const std::string name_space = class_namespace(klass);
    return name_space.empty() ? class_name(klass) : name_space + "." + class_name(klass);
}

uintptr_t Runtime::new_handle(void* object, bool weak) {
    if (!object) return 0;
    if (backend_ == Backend::Mono && mono_handles_v2_)
        return reinterpret_cast<uintptr_t>((weak ? mono_gchandle_new_weakref_v2_ : mono_gchandle_new_v2_)(object, 0));
    if (backend_ == Backend::Mono) {
        auto* create = weak ? mono_gchandle_new_weakref_ : mono_gchandle_new_;
        return create && mono_gchandle_get_target_ && mono_gchandle_free_ ? create(object, 0) : 0;
    }
    auto* create = weak ? il2cpp_gchandle_new_weakref_ : il2cpp_gchandle_new_;
    return create && il2cpp_gchandle_get_target_ && il2cpp_gchandle_free_ ? create(object, false) : 0;
}

void* Runtime::handle_target(uintptr_t handle) const {
    if (!handle) return nullptr;
    if (backend_ == Backend::Mono)
        return mono_handles_v2_ ? mono_gchandle_get_target_v2_(reinterpret_cast<void*>(handle)) : mono_gchandle_get_target_(uint32_t(handle));
    return il2cpp_gchandle_get_target_(handle);
}

void Runtime::free_handle(uintptr_t handle) {
    if (!handle) return;
    if (backend_ == Backend::Mono && mono_handles_v2_) mono_gchandle_free_v2_(reinterpret_cast<void*>(handle));
    else if (backend_ == Backend::Mono) mono_gchandle_free_(uint32_t(handle));
    else il2cpp_gchandle_free_(handle);
}

const wchar_t* Runtime::string_chars(const void* object, int& length) const {
    length = 0;
    if (!object) return nullptr;
    // x64 MonoString and Il2CppString: the object header, an int32 length, then the characters (mono_string_chars is
    // rcx+0x14 in the game's own runtime).
    constexpr int kChars = kObjectHeader + 4;
    const char* data = static_cast<const char*>(object);
    std::memcpy(&length, data + kObjectHeader, sizeof(length));
    if (length < 0 || length > 1 << 20) return nullptr;
    return reinterpret_cast<const wchar_t*>(data + kChars);
}

}  // namespace gyro4all::unity
