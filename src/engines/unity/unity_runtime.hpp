#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace gyro4all::unity {

enum class Backend { Waiting, Mono, Il2Cpp, Failed };

// CLR element types, the same numbers in Mono and IL2CPP.
inline constexpr int kTypeR4 = 0x0c, kTypeValueType = 0x11, kTypeClass = 0x12, kTypeGeneric = 0x15, kTypeObject = 0x1c, kTypeArray = 0x1d;
// The managed object header on x64 (class or vtable pointer, monitor), the same in Mono and IL2CPP. A field's offset
// counts it, in a value type's fields too: inside another object, a value type's field is at
// holder address + holder field's offset + field's offset - kObjectHeader.
inline constexpr uint32_t kObjectHeader = 16;

class Runtime {
public:
    Backend initialize();
    Backend backend() const { return backend_; }
    bool ready() const { return backend_ == Backend::Mono || backend_ == Backend::Il2Cpp; }

    void* find_class(const char* name_space, const char* name);
    void* find_method(void* klass, const char* name, int parameter_count);
    // The native function behind an extern managed method (Mono), or null.
    void* internal_call(void* method) const { return mono_lookup_internal_call_ && method ? mono_lookup_internal_call_(method) : nullptr; }
    // Managed call, without virtual dispatch: look the method up from the object's own class when it may be virtual. An
    // exception gives null and threw(); the first ones are logged with the method's name.
    void* invoke(void* method, void* object, void** arguments = nullptr);
    bool threw() const { return threw_; }  // the last invoke threw (a void method's null result says nothing)
    void* unbox(void* object) const;
    // A boxed value type returned by invoke, copied out.
    template <typename T>
    bool returned(void* boxed, T& value) const {
        void* data = unbox(boxed);
        if (!data) return false;
        value = *static_cast<T*>(data);
        return true;
    }
    void* object_class(void* object) const;

    void* find_objects_of_type(void* klass);
    size_t array_length(void* array);
    void* array_element(void* array, size_t index, size_t element_size = sizeof(void*));
    // An array class's element class, and what an element of a class takes in an array (it takes the element's class, not
    // the array's). Null or 0 when the exports are missing.
    void* element_class(void* array_class) const;
    size_t element_size(void* element_class) const;
    // The UnityEngine.Object behind this managed object still exists: a destroyed one keeps its managed side, whose calls
    // throw (Unity's "fake null"). Read from its m_CachedPtr, without a managed call; true when that field can't be found.
    bool alive(void* object);

    // The runtime's own reflection: exports that managed code stripping can't remove (optional: has_reflection).
    bool has_reflection() const { return reflection_; }
    void* find_field(void* klass, const char* name);  // base classes too
    void* next_field(void* klass, void** iterator);   // the class's own fields; start with *iterator = nullptr
    const char* field_name(void* field) const;
    uint32_t field_offset(void* field) const;
    bool field_is_static(void* field) const;
    int field_type(void* field) const;   // kType*
    void* field_class(void* field) const;  // the class of the field's type
    const char* class_name(void* klass) const;
    const char* class_namespace(void* klass) const;
    void* class_parent(void* klass) const;
    bool class_is_value_type(void* klass) const;  // false when it can't be told
    std::string class_full_name(void* klass) const;

    // GC handles: a weak one for an object kept between frames (0 when the exports are missing, or no object).
    uintptr_t new_handle(void* object, bool weak);
    void* handle_target(uintptr_t handle) const;
    void free_handle(uintptr_t handle);

    // A managed System.String's UTF-16 characters (read in place, valid while the object lives; null on failure).
    const wchar_t* string_chars(const void* object, int& length) const;

private:
    Backend initialize_mono(HMODULE module);
    Backend initialize_il2cpp(HMODULE module);
    Backend missing(const char* name);
    void* find_mono_class(const char* name_space, const char* name);
    void* find_il2cpp_class(const char* name_space, const char* name);

    Backend backend_ = Backend::Waiting;
    bool attached_ = false;
    bool reflection_ = false;
    bool threw_ = false;
    void* domain_ = nullptr;
    bool cached_ptr_looked_up_ = false;
    uint32_t cached_ptr_offset_ = 0;

    void* (*mono_get_root_domain_)() = nullptr;
    void* (*mono_thread_attach_)(void*) = nullptr;
    void (*mono_assembly_foreach_)(void (*)(void*, void*), void*) = nullptr;
    void* (*mono_assembly_get_image_)(void*) = nullptr;
    void* (*mono_class_from_name_)(void*, const char*, const char*) = nullptr;
    void* (*mono_class_get_method_from_name_)(void*, const char*, int) = nullptr;
    void* (*mono_class_get_parent_)(void*) = nullptr;
    void* (*mono_runtime_invoke_)(void*, void*, void**, void**) = nullptr;
    void* (*mono_object_unbox_)(void*) = nullptr;
    void* (*mono_object_get_class_)(void*) = nullptr;
    void* (*mono_class_get_type_)(void*) = nullptr;
    void* (*mono_type_get_object_)(void*, void*) = nullptr;
    size_t (*mono_array_length_)(void*) = nullptr;
    void* (*mono_lookup_internal_call_)(void*) = nullptr;
    void* (*mono_array_addr_with_size_)(void*, int, size_t) = nullptr;
    void* (*mono_class_get_field_from_name_)(void*, const char*) = nullptr;
    // Optional.
    void* (*mono_class_get_element_class_)(void*) = nullptr;
    int (*mono_class_array_element_size_)(void*) = nullptr;
    const char* (*mono_method_get_name_)(void*) = nullptr;
    void* (*mono_class_get_fields_)(void*, void**) = nullptr;
    const char* (*mono_field_get_name_)(void*) = nullptr;
    uint32_t (*mono_field_get_offset_)(void*) = nullptr;
    uint32_t (*mono_field_get_flags_)(void*) = nullptr;
    void* (*mono_field_get_type_)(void*) = nullptr;
    int (*mono_type_get_type_)(void*) = nullptr;
    void* (*mono_class_from_mono_type_)(void*) = nullptr;
    const char* (*mono_class_get_name_)(void*) = nullptr;
    const char* (*mono_class_get_namespace_)(void*) = nullptr;
    int (*mono_class_is_valuetype_)(void*) = nullptr;
    // The 32-bit handles of the classic API. Unity's later Mono (bdwgc) has pointer-sized handles and its classic API
    // truncates them (The Planet Crafter Demo crashed freeing one): its _v2 exports are used when it has all four.
    uint32_t (*mono_gchandle_new_)(void*, int) = nullptr;
    uint32_t (*mono_gchandle_new_weakref_)(void*, int) = nullptr;
    void* (*mono_gchandle_get_target_)(uint32_t) = nullptr;
    void (*mono_gchandle_free_)(uint32_t) = nullptr;
    void* (*mono_gchandle_new_v2_)(void*, int) = nullptr;
    void* (*mono_gchandle_new_weakref_v2_)(void*, int) = nullptr;
    void* (*mono_gchandle_get_target_v2_)(void*) = nullptr;
    void (*mono_gchandle_free_v2_)(void*) = nullptr;
    bool mono_handles_v2_ = false;

    void* (*il2cpp_domain_get_)() = nullptr;
    void* (*il2cpp_thread_attach_)(void*) = nullptr;
    void** (*il2cpp_domain_get_assemblies_)(const void*, size_t*) = nullptr;
    void* (*il2cpp_assembly_get_image_)(void*) = nullptr;
    void* (*il2cpp_class_from_name_)(void*, const char*, const char*) = nullptr;
    void* (*il2cpp_class_get_method_from_name_)(void*, const char*, int) = nullptr;
    void* (*il2cpp_class_get_parent_)(void*) = nullptr;
    void* (*il2cpp_runtime_invoke_)(void*, void*, void**, void**) = nullptr;
    void* (*il2cpp_object_unbox_)(void*) = nullptr;
    void* (*il2cpp_object_get_class_)(void*) = nullptr;
    void* (*il2cpp_class_get_type_)(void*) = nullptr;
    void* (*il2cpp_type_get_object_)(void*) = nullptr;
    size_t (*il2cpp_array_length_)(void*) = nullptr;
    void* (*il2cpp_array_addr_with_size_)(void*, int, size_t) = nullptr;
    void* (*il2cpp_class_get_field_from_name_)(void*, const char*) = nullptr;
    // Optional.
    void* (*il2cpp_class_get_element_class_)(void*) = nullptr;
    int (*il2cpp_class_array_element_size_)(void*) = nullptr;
    const char* (*il2cpp_method_get_name_)(void*) = nullptr;
    void* (*il2cpp_class_get_fields_)(void*, void**) = nullptr;
    const char* (*il2cpp_field_get_name_)(void*) = nullptr;
    uint32_t (*il2cpp_field_get_offset_)(void*) = nullptr;
    int (*il2cpp_field_get_flags_)(void*) = nullptr;
    void* (*il2cpp_field_get_type_)(void*) = nullptr;
    int (*il2cpp_type_get_type_)(void*) = nullptr;
    void* (*il2cpp_class_from_type_)(void*) = nullptr;
    const char* (*il2cpp_class_get_name_)(void*) = nullptr;
    const char* (*il2cpp_class_get_namespace_)(void*) = nullptr;
    bool (*il2cpp_class_is_valuetype_)(void*) = nullptr;
    uintptr_t (*il2cpp_gchandle_new_)(void*, bool) = nullptr;
    uintptr_t (*il2cpp_gchandle_new_weakref_)(void*, bool) = nullptr;
    void* (*il2cpp_gchandle_get_target_)(uintptr_t) = nullptr;
    void (*il2cpp_gchandle_free_)(uintptr_t) = nullptr;
};

const char* backend_name(Backend backend);

}  // namespace gyro4all::unity
