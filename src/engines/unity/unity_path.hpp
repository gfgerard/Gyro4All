#pragma once
// A profile's path from a managed object to one of its values ("a.b.c", "mode.GetData(@).pitchYaw"), walked with the
// runtime's own reflection and read or written in place through guarded memory access. Every step's type is checked,
// so a profile can't make Gyro4All copy a value of the wrong size. Lookups are kept per step and redone only when the
// object's class there changes, so walking a path every frame costs no name searches.
#include "unity_runtime.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::unity {

// Where a path ended.
struct FieldValue {
    uintptr_t address = 0;
    int type = 0;           // kType*
    void* klass = nullptr;  // the field's declared class
    bool copy = false;      // reached through a method returning a value type: writes wouldn't reach the game
    // The last part was a property (no field of that name): its getter's result, boxed for a value type. No address.
    bool called = false;
    void* returned = nullptr;
};

// A field holding an object, whatever its declared type says about it.
bool holds_object(Runtime& runtime, int type, void* value_class);
// The objects in an array, a List<> or a Dictionary<,>'s values, at most max. Empty for anything else, or a value type's.
bool is_container(Runtime& runtime, void* klass);
std::vector<void*> container_objects(Runtime& runtime, void* container, size_t max);

class ManagedPath {
public:
    ManagedPath() = default;
    explicit ManagedPath(std::vector<std::string> parts);
    bool empty() const { return parts_.empty(); }
    // The last part must be a field or getter. A "name()" part calls a method with no arguments, like a getter. A "method(@)" part calls a one-argument method on the object reached so far with
    // the root as its argument (methods and fields are found on base classes too). A "name[Class]" part is a field holding
    // an array, List<> or Dictionary<,>, and goes on with the first object of that class in it (its values, for a
    // dictionary). Quietly false while a step's object is null; a wrong name is logged once.
    // A name with no field of its own is read through its property's getter (get_<name>, no arguments) when the class has
    // one (gameObject, activeInHierarchy, InputAction.enabled): it goes on with what the getter returns, and at the end
    // gives that (FieldValue::called). Fields always come first, so a path that names a field reads it as before.
    bool resolve(Runtime& runtime, void* root, FieldValue& out);
    // The path's end as a bool: a bool field, or a getter returning one.
    bool read_bool(Runtime& runtime, void* root, bool& value);
    // The path's end as a number: a float, double or int field, or a getter returning a float.
    bool read_number(Runtime& runtime, void* root, double& value);
    // The path's end as an object (a field holding one, or a getter returning one), null otherwise.
    void* read_object(Runtime& runtime, void* root);

private:
    struct Step {
        void* klass = nullptr;  // the class this step was looked up on
        void* member = nullptr;
        uint32_t offset = 0;
        int type = 0;
        void* value_class = nullptr;
        bool getter = false;  // member is a property's getter
    };
    void complain(const std::string& text);

    std::vector<std::string> parts_;
    std::vector<Step> steps_;
    std::string complained_;
};

}  // namespace gyro4all::unity
