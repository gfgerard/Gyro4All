#pragma once
// RE Engine runtime reflection: the type database (TDB) in the game's memory and the via.Application module entries.
// Layouts follow REFramework's SDK (MIT; shared/sdk/RETypeDB.hpp tdb69/tdb70/tdb83, RETypeDefinition.hpp
// RETypeDefVersion69/71/83, Application.cpp) and tools/reEngine/tdbdump.py. TDB 69 and 70 (RE7/RE2/RE3 RT builds, RE8), 71
// (RE4 remake), 81 (MH Wilds) and 83 (RE Requiem, Pragmata). Every read goes through core/mem.hpp.
#include <cstdint>
#include <string>
#include <vector>

namespace gyro4all::re {

struct Tdb {
    uintptr_t header = 0;
    uint32_t version = 0;
    bool wide = false;          // TDB 71+: 19-bit type ids, a field's offset in its impl record (checked with 71, 81, 83)
    uint32_t type_size = 0x50;  // a RETypeDefinition: 0x48 bytes in TDB 71, 0x50 in the others
    uint32_t num_types = 0;
    uintptr_t types = 0;       // RETypeDefinition[num_types], type_size bytes each
    uintptr_t types_impl = 0;  // RETypeImpl[], 0x30 bytes each
    uintptr_t strings = 0;     // string pool
    uint32_t string_mask = 0;
    uintptr_t fields = 0;       // REField[], 8 bytes each
    uintptr_t fields_impl = 0;  // REFieldImpl[], 12 bytes each
};

// The TDB header ("TDB\0" + version) in the exe's writable sections, 0 if none. version receives its version.
uintptr_t find_tdb_header(uint32_t& version);
// The TDB once the game has initialised it (its tables are pointers then, not offsets from the header). False before
// that, or for an unsupported version.
bool read_tdb(uintptr_t header, Tdb& out);
bool tdb_supported(uint32_t version);

// The type index of namespace.name (top-level types only), 0 if not found.
uint32_t find_type(const Tdb& tdb, const char* name_space, const char* name);
// "namespace.Name", nested types as "Outer.Inner" (e.g. app.PlayerCamera.MaximumOperatable). "" if unreadable.
std::string type_name(const Tdb& tdb, uint32_t index);
uint32_t parent_type(const Tdb& tdb, uint32_t index);
// index is base_index or derives from it.
bool is_a(const Tdb& tdb, uint32_t index, uint32_t base_index);
// A member field's offset from the object's field base (see field_base), searched in the type and its parents.
// -1 if there is no such instance field.
int32_t field_offset(const Tdb& tdb, uint32_t index, const char* name);

// A type's own fields (not its parents'), in TDB order: name, offset from the field base, field type, static.
struct FieldDesc {
    std::string name;
    int32_t offset = -1;
    uint32_t type = 0;
    bool is_static = false;  // static or literal: no offset in the object
};
std::vector<FieldDesc> own_fields(const Tdb& tdb, uint32_t index);

// Managed objects: the type of an object (its REObjectInfo's type definition), 0 if obj isn't one.
uint32_t object_type(const Tdb& tdb, uintptr_t obj);
// Where an object's fields start (an int32 just before its REObjectInfo; 0x50 for behaviours, 0x10 for plain objects
// in RE7). -1 if unreadable.
int32_t field_base(uintptr_t obj);

// The game's static singleton slots: the exe's code loads each with "lea reg, [slot]; mov rax, 1<<63" (REFramework
// REGlobals). Each slot holds a pointer to a managed singleton (app.ObjectManager, app.GameManager...).
std::vector<uintptr_t> singleton_slots();
// Fallback for protected exes whose code hides that pattern (RE2's Xbox build): every pointer-sized slot in the game
// modules' writable sections that currently holds a managed object. Only filled slots are found, so call it again
// until the wanted singletons appear. Takes a few seconds.
std::vector<uintptr_t> managed_object_slots(const Tdb& tdb);

// The type's runtime type object (REType), 0 until the game created it.
uintptr_t runtime_type(const Tdb& tdb, uint32_t index);
// A native singleton's instance (e.g. via.Application), through its runtime type's getter. 0 if not available.
uintptr_t singleton_instance(uintptr_t runtime_type);

// via.Application's module entries: one per engine step of a frame (UpdateBehavior, LateUpdateBehavior,
// BeginRendering...), each with the function the engine calls for it.
struct AppEntry {
    uintptr_t address = 0;  // the entry in the table; its function pointer is at +8
    std::string name;
    uint16_t priority = 0;
};
std::vector<AppEntry> application_entries(const Tdb& tdb, uintptr_t application);

}  // namespace gyro4all::re
