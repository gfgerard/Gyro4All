#pragma once
// Frostbite runtime reflection: the engine's TypeInfo objects, found in the game's memory by shape, so no per-game
// offsets or code signatures. Each TypeInfo is a static object in the exe that points to its info data (whose first
// field is the type's name) and links to other registered TypeInfos. The reader finds the type "DataContainer"
// (every Frostbite game has it), works out where the info data and the links are from it, and walks the links. Two
// info data layouts (kind, size, parent, fields), checked on DataContainer before use:
// - 2021+ (Battlefield 2042, Dead Space 2023), as read by Frosty Tool Suite's SDK generator
//   (FrostyPlugin/Sdk/FBTypes/Bf2042.cs): links to the previous and next TypeInfo.
// - 2013 (Battlefield 4, worked out from the running game): one link, to the next TypeInfo of a single list whose head
//   is newer than DataContainer, so the types registered after it are found backwards (the image's qwords that point
//   to a known TypeInfo as their next). Array types have no name (named "<element>-Array" here).
// - 2017 (Mass Effect: Andromeda, worked out from the raw info data dump): the 2013 one with the array type after the
//   module; the same TypeInfo and single list. Arrays have their own name.
// Every read goes through core/mem.hpp.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gyro4all::fb {

// Kinds of type (Frosty's EbxFieldType; the info data's flags >> 5 & 0x1F in the 2021 layout, >> 4 in the 2013 one).
enum Kind : uint32_t { Struct = 2, Class = 3, Array = 4, Enum = 8, Float32 = 0x13, Float64 = 0x14 };

struct FieldDesc {
    std::string name;
    uint32_t offset = 0;
    uint16_t flags = 0;
    uintptr_t type = 0;  // the field's TypeInfo
};

struct TypeDesc {
    uintptr_t type = 0;  // the TypeInfo object
    uintptr_t info = 0;  // its info data
    std::string name, name_space;
    uint32_t kind = 0, size = 0, alignment = 0;
    uintptr_t parent = 0;      // classes: the base class's TypeInfo
    uintptr_t array_type = 0;  // the TypeInfo of an array of this type
    uintptr_t element = 0;     // arrays (2013 layout): the element's TypeInfo
    std::vector<FieldDesc> fields;
};

struct TypeTable {
    uint32_t info_offset = 0;      // TypeInfo -> info data
    std::vector<uint32_t> links;   // TypeInfo -> previous / next TypeInfo
    bool layout = false;           // the info data layout was recognised (kind, size, parent, fields are valid)
    uint32_t layout_year = 0;      // which one: 2013, 2017 or 2021 (0 = unknown)
    std::vector<TypeDesc> types;
    std::unordered_map<uintptr_t, size_t> by_type;  // TypeInfo -> index in types

    const TypeDesc* find(uintptr_t type) const {
        const auto it = by_type.find(type);
        return it == by_type.end() ? nullptr : &types[it->second];
    }
    const TypeDesc* find(const std::string& name) const;
    std::string type_name(uintptr_t type) const;
};

// Finds every TypeInfo of the exe. False until the game has registered its types (or when the shape isn't found);
// why = what is missing, for the log. Slow (scans the exe image a few times): attach thread only.
bool find_types(TypeTable& out, std::string& why);

// Code addresses of a class's getType() in the exe: `lea rax,[rip+type]; ret`. Slow (scans the exe image): attach
// thread only.
std::vector<uintptr_t> find_gettype(uintptr_t type);

// Addresses in the exe image where the bytes appear (code patterns, e.g. a family of small thunks). Slow: attach thread
// only.
std::vector<uintptr_t> find_pattern(const uint8_t* bytes, size_t n);

// Addresses of the 8-byte aligned qwords of the exe image whose value is in values (e.g. the vtable slots that hold a
// function). Slow: attach thread only.
std::vector<uintptr_t> find_in_image(const std::vector<uintptr_t>& values);

// Branches in the exe image to one of targets: `call rel32` / `jmp rel32` (E8 / E9) whose destination is a target, and
// `call` / `jmp qword ptr [rip+disp32]` (FF 15 / FF 25) whose slot is one. Pairs of (instruction address, target).
// Byte patterns only (data can match too: check the sites, e.g. against the unwind table). Slow: not on the game thread
// more than once.
std::vector<std::pair<uintptr_t, uintptr_t>> find_branches(const std::vector<uintptr_t>& targets);

// `lea r64,[rip+disp32]` (REX.W 8D, mod 00 rm 101) in the exe image whose address is one of targets. Pairs of
// (instruction address, target). Byte patterns only, like find_branches. Slow: not on the game thread
std::vector<std::pair<uintptr_t, uintptr_t>> find_lea_refs(const std::vector<uintptr_t>& targets);

// Writes the types to path as a readable SDK (class : parent, size, fields with offsets and types). Types whose name
// contains one of the comma-separated filter parts (any case) only, or all with an empty filter. Without a recognised
// layout, the raw info data instead, for working out a new Frostbite version's layout.
size_t dump_types(const TypeTable& table, const std::wstring& path, const std::string& filter);

}  // namespace gyro4all::fb
