#pragma once
// id Tech's type info, read from the running game's exe (the same tables tools/idTech/typeinfo.py reads from the file),
// static in the exe's .rdata/.data, in one of two layouts (both tried, the one that finds more classes wins):
// - id Tech 6/7 (DOOM 2016, Eternal): classTypeInfo_t { name, superName, size, 0, variables }, classVariableInfo_t
//   { type, ops, name, offset|size<<32, ... } every 0x48 bytes;
// - id Tech 8 (The Dark Ages): classTypeInfo_t { name, superName, hash, size, 0, variables }, classVariableInfo_t the
//   same first fields every 0x58 bytes.
// Plus MSVC RTTI (vtables of classes by name, the class name of an object) and the exe's sections. All reads are
// guarded (core/mem.hpp).
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gyro4all::idtech {

struct Section {
    uintptr_t begin = 0, end = 0;     // virtual range in memory
    uintptr_t init_end = 0;           // end of the part initialized from the file (static tables live there)
};

struct Image {
    uintptr_t base = 0, end = 0;
    Section text, rdata, data;
    bool load();  // the exe's PE headers in memory
    bool contains(uintptr_t p) const { return p >= base && p < end; }
};

struct ClassInfo {
    std::string super;
    uint64_t size = 0;
    uintptr_t variables = 0;
};

class TypeTable {
public:
    // Scans the exe's static data for the class tables (worker thread, once; ~100 ms).
    size_t load(const Image& image);
    // Offset of a field of cls or of its base classes, -1 when unknown. size (optional) receives its size.
    int64_t field(const std::string& cls, const std::string& name, uint32_t* size = nullptr) const;
    const ClassInfo* find(const std::string& cls) const;
    const char* layout() const { return stride_ == 0x58 ? "id Tech 8" : "id Tech 6/7"; }

private:
    size_t scan(const Image& image, int words, int size_at);
    const Image* image_ = nullptr;
    uintptr_t stride_ = 0x48;  // classVariableInfo_t entries
    std::unordered_map<std::string, ClassInfo> classes_;
    friend void add_class(TypeTable&, const char*, const char*, uint64_t, uintptr_t);
};

// RTTI: the vtable of the class with this decorated name (".?AVidGameLocal@@"), 0 if not found.
uintptr_t rtti_vtable(const Image& image, const char* decorated);
// The decorated class name of the object at p (".?AVidPlayer@@"), "" when it has no readable RTTI.
std::string rtti_name(const Image& image, uintptr_t p);
// First 8-aligned address in [begin, end) holding value, 0 if none (guarded scan).
uintptr_t find_qword(uintptr_t begin, uintptr_t end, uint64_t value);
// Up to max 16-aligned addresses in the process's committed private read-write memory (the heaps) holding value: the
// objects whose vtable is value. Returns how many were found; bytes receives the amount scanned.
size_t heap_find(uint64_t value, uintptr_t* out, size_t max, uint64_t* bytes = nullptr);

}  // namespace gyro4all::idtech
