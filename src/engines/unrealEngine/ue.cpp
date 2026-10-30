#include "ue.hpp"

#include "hooks.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <unordered_map>
#include <vector>

namespace gyro4all::ue {
namespace {

// Guarded memory access and module layout: core/mem.hpp. GUObjectArray lives in CoreUObject and FNamePool in Core
// (in monolithic builds both are in the exe), so they are found in the game modules' writable sections.
using mem::guard_filter;
using mem::guarded_copy;
using mem::guarded_store;
using mem::in_module;
using mem::module_offset;
using mem::plausible;
using mem::Range;
using mem::rd;
using mem::rdp;
using mem::writable_sections;

// ---------------------------------------------------------------- state

constexpr int kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18;
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;

uintptr_t g_obj_array = 0;   // TUObjectArray: Objects**, PreAllocated*, Max, Num, MaxChunks, NumChunks
// UE <= 4.19 (FFixedUObjectArray): one flat array, Objects*, MaxElements, NumElements (Days Gone, UE 4.11).
bool g_obj_flat = false;
int g_item_stride = 0;       // sizeof(FUObjectItem)
int g_item_object = 0;       // FUObjectItem::Object: +0, or +8 in UE 5.7 (a 64-bit flags/refcount field first)
uintptr_t g_name_blocks = 0; // FNamePool Blocks[]
bool g_header_len_high = true;  // FNameEntryHeader: Len in bits 6..15 (true) or 0..9
// WITH_CASE_PRESERVING_NAME builds: entries are ComparisonId (4 bytes), header (bIsWide bit 0, Len bits 1..15),
// characters, 4-byte aligned. FName is ComparisonIndex, Number, DisplayIndex (12 bytes), so UObject::Outer moves
// from +0x20 to +0x28.
bool g_name_case_preserving = false;
int g_obj_outer = 0x20;
// UE <= 4.22 has no FNamePool: TNameEntryArray (GNames) holds Chunks[128] of FNameEntry*[16384], then NumElements,
// NumChunks. FNameEntry: Index (name index << 1 | wide), HashNext, then the NUL-terminated characters.
uintptr_t g_name_chunks = 0;    // TNameEntryArray::Chunks
int g_name_text = 0x10;         // offset of the characters in an FNameEntry

int g_super = -1;        // UStruct::SuperStruct
int g_child_props = -1;  // UStruct::ChildProperties
int g_field_class = 0x08, g_field_next = -1, g_field_name = -1;
// Where a property's type name lives in its class object: FFieldClass::Name at +0 (UE 4.25+ FField properties) or
// the UClass's UObject name (UE <= 4.24 UProperty objects, e.g. Life is Strange Remastered).
int g_field_class_name = 0;
int g_array_dim = -1, g_elem_size = -1, g_prop_offset = -1, g_prop_size = -1;

uintptr_t g_class_class = 0;
bool g_ready = false;
ULONGLONG g_next_try = 0;
int g_array_tries = 0;  // init() attempts without GUObjectArray

std::unordered_map<uint64_t, std::string> g_names;
std::map<std::pair<uintptr_t, std::string>, Prop> g_props;

// ---------------------------------------------------------------- names

// A NUL-terminated string of up to 1024 characters, read in small pieces so a string near the end of a page
// still reads.
template <class Char>
bool read_cstring(uintptr_t address, std::basic_string<Char>& out) {
    Char piece[16];
    while (out.size() < 1024) {
        if (!guarded_copy(piece, address, sizeof(piece))) {
            // The rest of the piece may be unmapped: one character at a time.
            if (!guarded_copy(piece, address, sizeof(Char))) return false;
            if (!piece[0]) return !out.empty();
            out += piece[0];
            address += sizeof(Char);
            continue;
        }
        for (Char c : piece) {
            if (!c) return !out.empty();
            out += c;
        }
        address += sizeof(piece);
    }
    return false;
}

std::string legacy_entry_string(uint32_t index) {
    if (index >= 128 * 16384) return {};
    const uintptr_t chunk = rdp(g_name_chunks + (index / 16384) * 8ull);
    const uintptr_t entry = chunk ? rdp(chunk + (index % 16384) * 8ull) : 0;
    int32_t entry_index = 0;
    if (!entry || !rd(entry, entry_index)) return {};
    if (!(entry_index & 1)) {
        std::string s;
        return read_cstring(entry + g_name_text, s) ? s : std::string{};
    }
    std::wstring w;
    return read_cstring(entry + g_name_text, w) ? narrow(w) : std::string{};
}

std::string entry_string(uint32_t index) {
    if (g_name_chunks) return legacy_entry_string(index);
    const uint32_t block = index >> 16;
    uint32_t offset = (index & 0xFFFF) * (g_name_case_preserving ? 4 : 2);
    if (block >= 8192) return {};
    const uintptr_t base = rdp(g_name_blocks + block * 8ull, 2);
    if (!base) return {};
    if (g_name_case_preserving) offset += 4;  // past the ComparisonId
    uint16_t h = 0;
    if (!rd(base + offset, h)) return {};
    bool wide;
    uint32_t len;
    if (g_name_case_preserving) wide = h & 1, len = h >> 1;
    else if (g_header_len_high) wide = h & 1, len = h >> 6;
    else wide = (h >> 15) & 1, len = h & 0x3FF;
    if (len == 0 || len > 1024) return {};
    if (!wide) {
        std::string s(len, '\0');
        return guarded_copy(s.data(), base + offset + 2, len) ? s : std::string{};
    }
    std::wstring w(len, L'\0');
    if (!guarded_copy(w.data(), base + offset + 2, len * 2)) return {};
    return narrow(w);
}

std::string fname(uintptr_t address) {  // FName at address: ComparisonIndex, Number
    uint32_t pair[2] = {};
    if (!guarded_copy(pair, address, 8)) return {};
    const uint64_t key = (uint64_t(pair[1]) << 32) | pair[0];
    if (auto it = g_names.find(key); it != g_names.end()) return it->second;
    std::string s = entry_string(pair[0]);
    if (!s.empty() && pair[1] > 0) s += "_" + std::to_string(pair[1] - 1);
    g_names.emplace(key, s);
    return s;
}

// ---------------------------------------------------------------- object array

int32_t object_count() {
    int32_t n = 0;
    return rd(g_obj_array + (g_obj_flat ? 0x0C : 0x14), n) ? n : 0;
}

Obj object_at(int32_t i) {
    if (g_obj_flat) {
        const uintptr_t items = rdp(g_obj_array);
        return items ? rdp(items + uintptr_t(i) * g_item_stride + g_item_object) : 0;
    }
    const uintptr_t chunks = rdp(g_obj_array);
    if (!chunks) return 0;
    const uintptr_t chunk = rdp(chunks + (i / 65536) * 8ull);
    if (!chunk) return 0;
    return rdp(chunk + uintptr_t(i % 65536) * g_item_stride + g_item_object);
}

// FUObjectItems at `items`: items 1..16 are live UObjects whose InternalIndex matches. Returns the item stride
// (sizeof(FUObjectItem)), or -1. object_out: where the Object pointer is in an item (+0; +8 in UE 5.7, e.g. Valor
// Mortis, whose item starts with a 64-bit flags/refcount field).
int item_stride(uintptr_t items, int* object_out) {
    if (!items) return -1;
    for (int object : {0, 8}) {
        for (int stride : {0x18, 0x10, 0x20, 0x28, 0x30}) {
            if (object + 8 > stride) continue;
            bool ok = true;
            for (int i = 1; i <= 16 && ok; ++i) {
                const uintptr_t o = rdp(items + uintptr_t(i) * stride + object);
                int32_t idx = -1;
                uintptr_t vtable = 0;
                ok = o && rd(o + kObjIndex, idx) && idx == i && rd(o, vtable) && in_module(vtable);
            }
            if (!ok) continue;
            if (object_out) *object_out = object;
            return stride;
        }
    }
    return -1;
}

// FChunkedFixedUObjectArray at a: Objects**, PreAllocatedObjects*, MaxElements, NumElements, MaxChunks,
// NumChunks, with valid items in chunk 0. Returns the item stride, 0 if the header doesn't match, -1 if the header
// matches but the items don't.
int object_array_stride(uintptr_t a, uintptr_t* chunk0_out = nullptr, int* object_out = nullptr) {
    int32_t max = 0, num = 0, max_chunks = 0, num_chunks = 0;
    if (!rd(a + 0x10, max) || !rd(a + 0x14, num) || !rd(a + 0x18, max_chunks) || !rd(a + 0x1C, num_chunks)) return 0;
    if (num <= 1000 || num > max || max < 0x10000 || max > 0x10000000) return 0;
    if (max_chunks != (max + 65535) / 65536) return 0;
    if (num_chunks < (num + 65535) / 65536 || num_chunks > max_chunks) return 0;
    const uintptr_t chunks = rdp(a);
    const uintptr_t chunk0 = chunks ? rdp(chunks) : 0;
    if (chunk0_out) *chunk0_out = chunk0;
    return item_stride(chunk0, object_out);
}

// FFixedUObjectArray (UE <= 4.19) at a: Objects*, MaxElements, NumElements. Item stride, or <= 0.
int flat_object_array_stride(uintptr_t a, int* object_out = nullptr) {
    int32_t max = 0, num = 0;
    if (!rd(a + 0x08, max) || !rd(a + 0x0C, num)) return 0;
    if (num <= 1000 || num > max || max < 0x10000 || max > 0x10000000) return 0;
    return item_stride(rdp(a), object_out);
}

// diagnose: log every candidate that passes the header checks and why its items were rejected.
bool find_object_array(const std::vector<Range>& sections, bool diagnose = false) {
    int candidates = 0;
    for (const Range& r : sections) {
        for (uintptr_t a = r.begin; a + 0x20 <= r.end; a += 8) {
            uintptr_t chunk0 = 0;
            int object = 0;
            const int stride = object_array_stride(a, &chunk0, &object);
            if (stride > 0) {
                g_obj_array = a;
                g_item_stride = stride;
                g_item_object = object;
                return true;
            }
            if (const int flat = flat_object_array_stride(a, &object); flat > 0) {
                g_obj_array = a;
                g_obj_flat = true;
                g_item_stride = flat;
                g_item_object = object;
                return true;
            }
            if (stride == 0 || !diagnose || candidates >= 8) continue;
            ++candidates;
            std::string items;
            for (int i = 0; i < 4 && chunk0; ++i) {
                char buffer[96];
                const uintptr_t o = rdp(chunk0 + uintptr_t(i) * 0x18);
                int32_t idx = -1;
                uintptr_t vtable = 0;
                rd(o + kObjIndex, idx);
                rd(o, vtable);
                std::snprintf(buffer, sizeof buffer, " [%d] obj %p idx %d vtable %s", i, reinterpret_cast<void*>(o), idx,
                              vtable ? module_offset(vtable).c_str() : "-");
                items += buffer;
            }
            log("ue: object array candidate %s: chunk0 %p (stride 0x18):%s", module_offset(a).c_str(),
                reinterpret_cast<void*>(chunk0), items.c_str());
        }
    }
    return false;
}

// Some builds (Gears of War: E-Day, UE 5.6) allocate FUObjectArray on the heap and keep only an
// XOR-encoded pointer to it in a global; the key is an immediate in the code that reads it:
//     mov r64, [rip+global] ; mov r64, imm64 (key) ; xor r64, r64
// The exe's code is scanned once for that idiom; each (global, key) is decoded on every retry (the
// singleton may not exist yet) and accepted only if it holds a valid ObjObjects within its 0xB8 bytes.
struct EncodedPtr {
    uintptr_t global;
    uint64_t key;
    uintptr_t code;
};
constexpr size_t kMaxEncoded = 8192;
EncodedPtr g_encoded[kMaxEncoded];
size_t g_encoded_count = 0;
bool g_encoded_scanned = false;
uint64_t g_code_bytes = 0;      // scanned by scan_code_for_encoded_pointers (diagnostics)
unsigned g_code_regions = 0, g_code_skipped = 0, g_code_faults = 0;

void add_encoded(uintptr_t global, uint64_t key, uintptr_t code) {
    for (size_t i = 0; i < g_encoded_count; ++i)
        if (g_encoded[i].global == global && g_encoded[i].key == key) return;
    if (g_encoded_count < kMaxEncoded) g_encoded[g_encoded_count++] = {global, key, code};
}

// Raw scan of one committed, readable range (SEH-guarded; no C++ objects with destructors here).
void scan_xor_loads(uintptr_t begin, uintptr_t end) {
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(begin);
        const size_t n = end - begin;
        for (size_t i = 0; i + 20 <= n; ++i) {
            if (p[i + 1] != 0x8B || (p[i] != 0x48 && p[i] != 0x4C) || (p[i + 2] & 0xC7) != 0x05) continue;
            if ((p[i + 7] != 0x48 && p[i + 7] != 0x49) || p[i + 8] < 0xB8 || p[i + 8] > 0xBF) continue;
            if ((p[i + 17] & 0xF2) != 0x48 || (p[i + 18] != 0x33 && p[i + 18] != 0x31)) continue;
            int32_t disp = 0;
            uint64_t key = 0;
            std::memcpy(&disp, p + i + 3, 4);
            std::memcpy(&key, p + i + 9, 8);
            if (key == 0) continue;
            add_encoded(begin + i + 7 + intptr_t(disp), key, begin + i);
        }
    } __except (guard_filter(GetExceptionInformation())) {
        ++g_code_faults;
    }
}

void scan_code_for_encoded_pointers() {
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t a = base + sec->VirtualAddress;
            const uintptr_t end = std::min<uintptr_t>(a + sec->Misc.VirtualSize, image_end);
            while (a < end) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
                const uintptr_t region_end =
                    std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, end);
                const DWORD prot = mbi.Protect & 0xFF;
                const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                                      (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                                       prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_READONLY ||
                                       prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);
                if (readable) {
                    scan_xor_loads(a, region_end);
                    g_code_bytes += region_end - a;
                    ++g_code_regions;
                } else {
                    ++g_code_skipped;
                }
                a = region_end;
            }
        }
    }
}

bool find_encoded_object_array() {
    if (!g_encoded_scanned) {
        g_encoded_scanned = true;
        const ULONGLONG start = GetTickCount64();
        scan_code_for_encoded_pointers();
        log("ue: %zu XOR-encoded pointer loads in the game code (%llu ms; %.1f MB in %u regions, %u regions not "
            "readable, %u faults)",
            g_encoded_count, static_cast<unsigned long long>(GetTickCount64() - start), double(g_code_bytes) / 1048576.0,
            g_code_regions, g_code_skipped, g_code_faults);
    }
    for (size_t i = 0; i < g_encoded_count; ++i) {
        uint64_t encoded = 0;
        if (!rd(g_encoded[i].global, encoded) || !encoded) continue;
        const uintptr_t object_array = uintptr_t(encoded ^ g_encoded[i].key);
        if (!plausible(object_array)) continue;
        for (uintptr_t off = 0; off + 0x20 <= 0xB8; off += 8) {
            if (object_array_stride(object_array + off) <= 0) continue;
            g_obj_array = object_array + off;
            g_item_stride = object_array_stride(g_obj_array, nullptr, &g_item_object);
            log("ue: FUObjectArray %p decoded from %s ^ key (code at %s); ObjObjects at +0x%llX",
                reinterpret_cast<void*>(object_array), module_offset(g_encoded[i].global).c_str(),
                module_offset(g_encoded[i].code).c_str(), static_cast<unsigned long long>(off));
            return true;
        }
    }
    return false;
}


// Last resort, independent of how the code is compiled or protected: the engine's FUObjectArray lives
// somewhere in the process heap, so every committed private read-write region is scanned for a valid
// ObjObjects (same checks as above). Several GB, so it runs on its own thread, once every 30 s at most,
// and the game thread picks the result up in init().
std::atomic<uintptr_t> g_heap_array{0};
std::atomic<bool> g_heap_scan_running{false};
int g_heap_scan_runs = 0;
ULONGLONG g_next_heap_scan = 0;

// Raw prefilter over one region (SEH-guarded); full validation through object_array_stride().
uintptr_t scan_heap_region(uintptr_t begin, uintptr_t end) {
    __try {
        for (uintptr_t a = begin; a + 0x20 <= end; a += 8) {
            const auto* q = reinterpret_cast<const int32_t*>(a + 0x10);  // Max, Num, MaxChunks, NumChunks
            const int32_t max = q[0], num = q[1], max_chunks = q[2], num_chunks = q[3];
            if (num_chunks <= 0 || num_chunks > max_chunks || max_chunks > 4096) continue;
            if (max < 0x10000 || max_chunks != (max + 65535) / 65536 || num <= 1000 || num > max) continue;
            if (object_array_stride(a) > 0) return a;
        }
    } __except (guard_filter(GetExceptionInformation())) {
    }
    return 0;
}

DWORD WINAPI heap_scan_thread(void*) {
    const ULONGLONG start = GetTickCount64();
    uint64_t bytes = 0;
    unsigned regions = 0;
    uintptr_t found = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && !found;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            found = scan_heap_region(base, end);
            bytes += mbi.RegionSize;
            ++regions;
        }
        a = end;
    }
    log("ue: heap scan %s (%.2f GB in %u regions, %llu ms)", found ? "found ObjObjects" : "found nothing",
        double(bytes) / (1024.0 * 1024.0 * 1024.0), regions, static_cast<unsigned long long>(GetTickCount64() - start));
    if (found) g_heap_array = found;
    g_heap_scan_running = false;
    return 0;
}

// Starts a background heap scan if none is running and the last one was long enough ago.
void start_heap_scan() {
    const ULONGLONG now = GetTickCount64();
    if (g_heap_scan_running || now < g_next_heap_scan || g_heap_scan_runs >= 10) return;
    g_next_heap_scan = now + 30000;
    ++g_heap_scan_runs;
    g_heap_scan_running = true;
    if (HANDLE thread = CreateThread(nullptr, 0, &heap_scan_thread, nullptr, 0, nullptr)) CloseHandle(thread);
    else g_heap_scan_running = false;
}

void log_sections() {
    for (HMODULE module : hooks::game_modules()) {
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            char name[9] = {};
            std::memcpy(name, sec->Name, 8);
            const DWORD c = sec->Characteristics;
            log("ue: section %-8s rva 0x%08lX size 0x%08lX %c%c%c", name, sec->VirtualAddress, sec->Misc.VirtualSize,
                (c & IMAGE_SCN_MEM_READ) ? 'R' : '-', (c & IMAGE_SCN_MEM_WRITE) ? 'W' : '-', (c & IMAGE_SCN_MEM_EXECUTE) ? 'X' : '-');
        }
    }
}

bool find_name_pool(const std::vector<Range>& sections) {
    for (const Range& r : sections) {
        for (uintptr_t a = r.begin + 8; a + 8 <= r.end; a += 8) {
            const uintptr_t block0 = rdp(a, 2);
            if (!block0) continue;
            char bytes[32] = {};
            if (!guarded_copy(bytes, block0, sizeof(bytes))) continue;
            // Case-preserving names (WITH_CASE_PRESERVING_NAME, PAYDAY 3): each entry starts with its 4-byte
            // ComparisonId, then a header with bIsWide in bit 0 and Len above it; entries are 4-byte aligned.
            const bool case_preserving = std::memcmp(bytes + 6, "None", 4) == 0 &&
                                         std::memcmp(bytes + 0x12, "ByteProperty", 12) == 0;
            if (!case_preserving &&
                (std::memcmp(bytes + 2, "None", 4) != 0 || std::memcmp(bytes + 8, "ByteProperty", 12) != 0))
                continue;
            uint32_t current_block = 0;
            if (!rd(a - 8, current_block) || current_block >= 8192) continue;
            if (!rdp(a + current_block * 8ull, 2)) continue;
            // The allocator fills blocks in order: every block up to CurrentBlock exists, none after it. Hogwarts
            // Legacy has another table that also starts with block 0 (CurrentBlock 0, block 1 null, blocks 2+ set)
            // before the real pool in .data; taking it decoded every name past block 0 as garbage.
            bool in_order = true;
            for (uint32_t b = 1; b <= current_block && in_order; ++b) in_order = rdp(a + b * 8ull, 2) != 0;
            for (uint32_t b = current_block + 1; b <= current_block + 4 && b < 8192 && in_order; ++b)
                in_order = rdp(a + b * 8ull, 2) == 0;
            if (!in_order) continue;
            if (case_preserving) {
                const uint16_t h = uint16_t(uint8_t(bytes[4]) | (uint8_t(bytes[5]) << 8));
                if (h != 4 << 1) continue;
                g_name_case_preserving = true;
                g_obj_outer = 0x28;
                g_name_blocks = a;
                return true;
            }
            const uint16_t h = uint16_t(uint8_t(bytes[0]) | (uint8_t(bytes[1]) << 8));
            if ((h >> 6) == 4 && !(h & 1)) g_header_len_high = true;
            else if ((h & 0x3FF) == 4) g_header_len_high = false;
            else continue;
            g_name_blocks = a;
            return true;
        }
    }
    return false;
}

// TNameEntryArray (UE <= 4.22) at `chunks`: names 0 and 1 are None and ByteProperty, NumElements/NumChunks follow
// the 128 chunk pointers. Sets the characters' offset in an entry (+0x12 in Days Gone: 2 more bytes after HashNext).
bool legacy_names_at(uintptr_t chunks) {
    const uintptr_t chunk0 = rdp(chunks);
    if (!chunk0) return false;
    const uintptr_t e0 = rdp(chunk0), e1 = rdp(chunk0 + 8);
    if (!e0 || !e1) return false;
    int32_t num = 0, num_chunks = 0;
    if (!rd(chunks + 128 * 8, num) || !rd(chunks + 128 * 8 + 4, num_chunks)) return false;
    if (num <= 1000 || num > 128 * 16384 || num_chunks < (num + 16383) / 16384 || num_chunks > 128) return false;
    for (int off : {0x10, 0x0C, 0x08, 0x14, 0x18, 0x12}) {
        char a[5] = {}, b[13] = {};
        if (!guarded_copy(a, e0 + off, sizeof(a)) || !guarded_copy(b, e1 + off, sizeof(b))) continue;
        if (std::memcmp(a, "None", 5) != 0 || std::memcmp(b, "ByteProperty", 13) != 0) continue;
        g_name_text = off;
        return true;
    }
    return false;
}

// GNames: a global TNameEntryArray, or (stock 4.21/4.22) a global pointer to a heap one.
bool find_legacy_names(const std::vector<Range>& sections) {
    for (const Range& r : sections) {
        for (uintptr_t a = r.begin; a + 128 * 8 + 8 <= r.end; a += 8) {
            if (legacy_names_at(a)) return g_name_chunks = a, true;
            const uintptr_t p = rdp(a);
            if (p && legacy_names_at(p)) return g_name_chunks = p, true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- reflection layout

std::string field_name(uintptr_t field) { return fname(field + g_field_name); }

std::string field_type(uintptr_t field) {
    const uintptr_t cls = rdp(field + g_field_class);
    return cls ? fname(cls + g_field_class_name) : std::string{};
}

Obj find_class(const char* name) {
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + kObjClass) == g_class_class && fname(o + kObjName) == name) return o;
    }
    return 0;
}

uintptr_t find_field(uintptr_t first, int next_off, int name_off, const char* name, int limit = 400) {
    uintptr_t f = first;
    for (int i = 0; f && i < limit; ++i) {
        if (fname(f + name_off) == name) return f;
        f = rdp(f + next_off);
    }
    return 0;
}

// Once per session, when the core classes aren't found: what the object array holds instead (Hogwarts Legacy).
void log_missing_classes(Obj object_cls, Obj actor_cls, Obj controller_cls) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    const int32_t n = object_count();
    int32_t classes = 0;
    std::string sample;
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (!o || rdp(o + kObjClass) != g_class_class) continue;
        if (++classes <= 12) sample += (sample.empty() ? "" : ", ") + fname(o + kObjName);
    }
    log("ue: core classes missing:%s%s%s; %d of %d objects are classes (first: %s)", object_cls ? "" : " Object",
        actor_cls ? "" : " Actor", controller_cls ? "" : " Controller", classes, n, sample.c_str());
    for (const char* name : {"Object", "Actor", "Controller"}) {
        for (int32_t i = 0; i < n; ++i) {
            const Obj o = object_at(i);
            if (!o || fname(o + kObjName) != name) continue;
            const Obj cls = rdp(o + kObjClass);
            log("ue:   an object named %s is #%d, class %s (0x%llX; Class is 0x%llX)", name, i,
                cls ? fname(cls + kObjName).c_str() : "(null)", static_cast<unsigned long long>(cls),
                static_cast<unsigned long long>(g_class_class));
            break;
        }
    }
}

bool detect_layout() {
    // UClass "Class" is its own class.
    const int32_t n = object_count();
    for (int32_t i = 0; i < n && !g_class_class; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + kObjClass) == o && fname(o + kObjName) == "Class") g_class_class = o;
    }
    if (!g_class_class) return log("ue: UClass 'Class' not found"), false;

    const Obj object_cls = find_class("Object"), actor_cls = find_class("Actor"), controller_cls = find_class("Controller");
    if (!object_cls || !actor_cls || !controller_cls) {
        log_missing_classes(object_cls, actor_cls, controller_cls);
        return log("ue: core classes not found"), false;
    }

    for (int off = 0x28; off <= 0x80 && g_super < 0; off += 8)
        if (rdp(actor_cls + off) == object_cls && rdp(controller_cls + off) == actor_cls) g_super = off;
    if (g_super < 0) return log("ue: SuperStruct offset not found"), false;

    // ChildProperties + FField::Next/NamePrivate: walk Controller's own properties to "ControlRotation".
    uintptr_t control_rotation = 0;
    for (int child = 0x38; child <= 0x80 && !control_rotation; child += 8) {
        const uintptr_t first = rdp(controller_cls + child);
        if (!first || child == g_super) continue;
        for (int name_off : {0x28, 0x20, 0x18, 0x30}) {
            for (int next_off : {0x20, 0x18, 0x10, 0x28}) {
                if (next_off == name_off) continue;
                if (uintptr_t f = find_field(first, next_off, name_off, "ControlRotation")) {
                    control_rotation = f;
                    g_child_props = child;
                    g_field_next = next_off;
                    g_field_name = name_off;
                    break;
                }
            }
            if (control_rotation) break;
        }
    }
    if (!control_rotation) {
        static bool logged = false;  // retried every 5 s; early in startup this is normal, so say it once
        if (!logged) log("ue: property layout not found yet (retrying every 5 s)");
        logged = true;
        return false;
    }
    const uintptr_t pawn = find_field(rdp(controller_cls + g_child_props), g_field_next, g_field_name, "Pawn");

    // FField::ClassPrivate, the FFieldClass* whose first member is its FName ("StructProperty"...). +0x08 in
    // stock UE 4.25-5.x, elsewhere in some builds (Life is Strange Remastered: Next 0x28, Name 0x18). Without it
    // every property type reads as "" and the probe finds no values.
    // UE <= 4.24 has no FField: properties are UObjects (UProperty) chained through UField::Next in
    // UStruct::Children, so the "FField" layout found above is the UObject one (Name 0x18, Next 0x28) and the type
    // is the UClass at +0x10, named by its own UObject name.
    g_field_class = -1;
    for (int name_in_class : {0, kObjName}) {
        for (int off = 0x08; off <= 0x40 && g_field_class < 0; off += 8) {
            if (off == g_field_next || off == g_field_name) continue;
            const uintptr_t cls = rdp(control_rotation + off);
            if (cls && fname(cls + name_in_class) == "StructProperty" &&
                (!pawn || fname(rdp(pawn + off) + name_in_class) == "ObjectProperty")) {
                g_field_class = off;
                g_field_class_name = name_in_class;
            }
        }
        if (g_field_class >= 0) break;
    }
    if (g_field_class < 0) {
        g_field_class = 0x08;
        g_field_class_name = 0;
        log("ue: property class offset not found, assuming 0x08 (property types may read empty)");
    }

    // FProperty: ArrayDim, ElementSize, Offset_Internal, and the size of FProperty (where subclass data starts).
    auto valid_rotator = [](uintptr_t f, int dim_off, int elem_off, int offset_off) {
        int32_t dim = 0, elem = 0, offset = 0;
        return rd(f + dim_off, dim) && dim == 1 && rd(f + elem_off, elem) && (elem == 12 || elem == 24) &&
               rd(f + offset_off, offset) && offset >= 0x28 && offset < 0x4000;
    };
    for (int elem_off = g_field_name + 0x10; elem_off <= g_field_name + 0x30 && g_elem_size < 0; elem_off += 4) {
        int32_t pawn_elem = 0;
        if (pawn && (!rd(pawn + elem_off, pawn_elem) || pawn_elem != 8)) continue;
        for (int offset_off = elem_off + 4; offset_off <= elem_off + 0x20; offset_off += 4) {
            if (!valid_rotator(control_rotation, elem_off - 4, elem_off, offset_off)) continue;
            // Validate FProperty size: a StructProperty's UScriptStruct follows it and is named "Rotator".
            // sizeof(FProperty) is pointer-aligned (0x78 in UE 4.25-5.x).
            for (int size = (offset_off + 0x20 + 7) & ~7; size <= offset_off + 0x40; size += 8) {
                const uintptr_t st = rdp(control_rotation + size);
                if (st && fname(st + kObjName) == "Rotator") {
                    g_array_dim = elem_off - 4;
                    g_elem_size = elem_off;
                    g_prop_offset = offset_off;
                    g_prop_size = size;
                    break;
                }
            }
            if (g_prop_size > 0) break;
        }
    }
    if (g_prop_size < 0) return log("ue: FProperty layout not found"), false;

    log("ue: layout (%s) SuperStruct=0x%X ChildProperties=0x%X Field.Class=0x%X Field.Next=0x%X Field.Name=0x%X "
        "Property.ElementSize=0x%X Offset_Internal=0x%X sizeof(Property)=0x%X ControlRotation type=%s",
        g_field_class_name ? "UProperty objects, UE <= 4.24" : "FField properties, UE 4.25+", g_super, g_child_props,
        g_field_class, g_field_next, g_field_name, g_elem_size, g_prop_offset, g_prop_size,
        field_type(control_rotation).c_str());
    return true;
}

}  // namespace

// ---------------------------------------------------------------- public API

bool ready() { return g_ready; }

bool init() {
    if (g_ready) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_try) return false;
    g_next_try = now + 1000;

    const std::vector<Range> sections = writable_sections();
    if (!g_obj_array) {
        bool found = find_object_array(sections);
        // Heap FUObjectArray behind an XOR-encoded global (cheap after the one-time code scan).
        if (!found && g_array_tries >= 2) found = find_encoded_object_array();
        // Otherwise look for it in the whole heap, in the background.
        if (!found && g_heap_array) {
            g_obj_array = g_heap_array;
            g_item_stride = object_array_stride(g_obj_array, nullptr, &g_item_object);
            found = g_item_stride > 0;
            if (!found) g_obj_array = g_heap_array = 0;
        }
        if (!found && g_array_tries >= 3) start_heap_scan();
        // Not in the plain data sections after ~10 s: protected exes can merge data into writable+executable
        // sections. Scan those every 10 s (slow), and the first time log the section table and the candidates.
        if (!found && ++g_array_tries % 10 == 0) {
            const bool diagnose = g_array_tries == 10;
            if (diagnose) log_sections();
            found = find_object_array(writable_sections(true), diagnose);
            if (!found && diagnose) log("ue: GUObjectArray not found yet (retrying every 10 s)");
        }
        if (found)
            log("ue: GUObjectArray.ObjObjects at %s (%s, item stride 0x%X, object at +0x%X, %d objects)",
                module_offset(g_obj_array).c_str(), g_obj_flat ? "flat, UE 4.19 or older" : "chunked", g_item_stride,
                g_item_object, object_count());
    }
    if (!g_name_blocks && find_name_pool(sections))
        log("ue: FNamePool.Blocks at %s (header len %s)", module_offset(g_name_blocks).c_str(),
            g_name_case_preserving ? "case-preserving entries, Outer +0x28"
            : g_header_len_high    ? "high bits"
                                   : "low bits");
    // UE <= 4.22: no pool, the older name table. Only once the objects are found, so a pool that a later try
    // would find (4.23+) isn't passed over.
    if (!g_name_blocks && !g_name_chunks && g_obj_array && find_legacy_names(sections))
        log("ue: no FNamePool; GNames (TNameEntryArray, UE 4.22 or older) chunks at %s, characters at entry +0x%X",
            module_offset(g_name_chunks).c_str(), g_name_text);
    if (!g_obj_array || (!g_name_blocks && !g_name_chunks)) return false;
    if (!detect_layout()) {
        g_next_try = now + 5000;
        g_class_class = 0;
        g_super = g_child_props = g_field_next = g_field_name = -1;
        g_array_dim = g_elem_size = g_prop_offset = g_prop_size = -1;
        return false;
    }
    g_ready = true;
    return true;
}

std::string name_of(Obj o) { return o ? fname(o + kObjName) : std::string{}; }

Obj class_of(Obj o) { return o ? rdp(o + kObjClass) : 0; }

bool is_cdo(Obj o) {
    uint32_t flags = 0;
    return !rd(o + kObjFlags, flags) || (flags & (RF_ClassDefaultObject | RF_ArchetypeObject));
}

bool is_a(Obj o, const char* class_name) {
    Obj c = class_of(o);
    for (int i = 0; c && i < 64; ++i) {
        if (name_of(c) == class_name) return true;
        c = rdp(c + g_super);
    }
    return false;
}

bool is_a_like(Obj o, const char* part) {
    std::string lower_part = part;
    for (char& ch : lower_part) ch = (char)std::tolower((unsigned char)ch);
    Obj c = class_of(o);
    for (int i = 0; c && i < 64; ++i) {
        std::string name = name_of(c);
        for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
        if (name.find(lower_part) != std::string::npos) return true;
        c = rdp(c + g_super);
    }
    return false;
}

Obj find_instance(const char* class_name) {
    if (!g_ready) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && !is_cdo(o) && is_a(o, class_name)) return o;
    }
    return 0;
}

Obj outer_of(Obj o) { return o ? rdp(o + g_obj_outer) : 0; }

Obj find_subobject(Obj outer, const char* name) {
    if (!g_ready || !outer) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + g_obj_outer) == outer && fname(o + kObjName) == name) return o;
    }
    return 0;
}

Obj find_subobject_of_class(Obj outer, const char* class_name) {
    if (!g_ready || !outer) return 0;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (o && rdp(o + g_obj_outer) == outer && !is_cdo(o) && is_a(o, class_name)) return o;
    }
    return 0;
}

Prop find_prop(Obj cls, const char* name) {
    if (!g_ready || !cls) return {};
    const auto key = std::make_pair(cls, std::string(name));
    if (auto it = g_props.find(key); it != g_props.end()) return it->second;
    Prop p;
    // "Struct.Member" (any depth): a property nested in struct properties, at the sum of their offsets
    // (Jedi: Fallen Order's RsCameraManager ModeOutputs.Rotation). Found by reflection, so it survives game updates.
    if (const char* dot = std::strchr(name, '.')) {
        const Prop outer = find_prop(cls, std::string(name, dot).c_str());
        const Obj st = outer && outer.type == "StructProperty" ? struct_type(outer) : 0;
        const Prop inner = st ? find_prop(st, dot + 1) : Prop{};
        if (inner) p = inner, p.offset += outer.offset;
        g_props.emplace(key, p);
        return p;
    }
    Obj c = cls;
    for (int depth = 0; c && depth < 64 && !p.field; ++depth) {
        if (uintptr_t f = find_field(rdp(c + g_child_props), g_field_next, g_field_name, name)) {
            p.field = f;
            rd(f + g_prop_offset, p.offset);
            rd(f + g_elem_size, p.element_size);
            p.type = field_type(f);
        }
        c = rdp(c + g_super);
    }
    g_props.emplace(key, p);
    return p;
}

Prop prop_of(Obj o, const char* name) { return find_prop(class_of(o), name); }

Obj read_obj(Obj o, const char* name) {
    const Prop p = prop_of(o, name);
    return p ? rdp(o + p.offset) : 0;
}

bool read_bool(Obj o, const char* name, bool fallback) {
    const Prop p = prop_of(o, name);
    if (!p) return fallback;
    uint8_t info[4] = {};  // FieldSize, ByteOffset, ByteMask, FieldMask
    if (p.type != "BoolProperty" || !guarded_copy(info, p.field + g_prop_size, 4)) return fallback;
    uint8_t byte = 0;
    if (!rd(o + p.offset + info[1], byte)) return fallback;
    return (byte & info[3]) != 0;
}

Obj struct_type(const Prop& p) { return p ? rdp(p.field + g_prop_size) : 0; }

bool read_rotator(uintptr_t address, int32_t element_size, double& pitch, double& yaw, double& roll) {
    if (element_size == 24) {
        double v[3];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        pitch = v[0], yaw = v[1], roll = v[2];
        return true;
    }
    if (element_size == 12) {
        float v[3];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        pitch = v[0], yaw = v[1], roll = v[2];
        return true;
    }
    return false;
}

bool add_rotator(uintptr_t address, int32_t element_size, double pitch, double yaw) {
    if (element_size == 24) {
        double v[2];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        v[0] += pitch, v[1] += yaw;
        return guarded_store(address, v, sizeof(v));
    }
    if (element_size == 12) {
        float v[2];
        if (!guarded_copy(v, address, sizeof(v))) return false;
        v[0] += float(pitch), v[1] += float(yaw);
        return guarded_store(address, v, sizeof(v));
    }
    return false;
}

constexpr double kRadToDeg = 57.29577951308232;

bool read_matrix_rotation(uintptr_t address, double& pitch, double& yaw, double& roll) {
    float m[12];  // rows X, Y, Z (each x, y, z, w)
    if (!guarded_copy(m, address, sizeof(m))) return false;
    if (!std::isfinite(m[0]) || !std::isfinite(m[1]) || !std::isfinite(m[2]) || std::abs(m[2]) > 1.001f) return false;
    pitch = std::asin(std::clamp(double(m[2]), -1.0, 1.0)) * kRadToDeg;
    yaw = std::atan2(double(m[1]), double(m[0])) * kRadToDeg;
    roll = std::atan2(-double(m[6]), double(m[10])) * kRadToDeg;
    return true;
}

bool add_matrix_rotation(uintptr_t address, double pitch, double yaw) {
    double p = 0, y = 0, r = 0;
    if (!read_matrix_rotation(address, p, y, r)) return false;
    p = std::clamp(p + pitch, -89.0, 89.0) / kRadToDeg, y = (y + yaw) / kRadToDeg, r /= kRadToDeg;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    const float rows[3][3] = {
        {float(cp * cy), float(cp * sy), float(sp)},
        {float(sr * sp * cy - cr * sy), float(sr * sp * sy + cr * cy), float(-sr * cp)},
        {float(-(cr * sp * cy + sr * sy)), float(cy * sr - cr * sp * sy), float(cr * cp)},
    };
    for (int i = 0; i < 3; ++i)
        if (!guarded_store(address + i * 16, rows[i], sizeof(rows[i]))) return false;
    return true;
}

void dump_props(Obj o, const char* stop_at_class) {
    Obj c = class_of(o);
    for (int depth = 0; c && depth < 64; ++depth) {
        const std::string cname = name_of(c);
        if (cname == stop_at_class) break;
        int count = 0;
        for (uintptr_t f = rdp(c + g_child_props); f && count < 2000; f = rdp(f + g_field_next), ++count) {
            int32_t offset = -1, size = 0;
            rd(f + g_prop_offset, offset);
            rd(f + g_elem_size, size);
            const std::string name = field_name(f);
            const std::string value = value_string(o, name.c_str());
            log("  %s.%s %s @0x%X size %d%s%s", cname.c_str(), name.c_str(), field_type(f).c_str(), offset, size,
                value.empty() ? "" : " = ", value.c_str());
        }
        c = rdp(c + g_super);
    }
}

std::string value_string(Obj o, const char* name) {
    const Prop p = prop_of(o, name);
    if (!p) return {};
    const uintptr_t a = o + p.offset;
    char buffer[96] = {};
    if (p.type == "FloatProperty") {
        float v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    } else if (p.type == "DoubleProperty") {
        double v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%.3f", v);
    } else if (p.type == "IntProperty") {
        int32_t v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%d", v);
    } else if (p.type == "ByteProperty" || p.type == "EnumProperty") {
        uint8_t v;
        if (rd(a, v)) std::snprintf(buffer, sizeof(buffer), "%u", v);
    } else if (p.type == "BoolProperty") {
        return read_bool(o, name, false) ? "true" : "false";
    } else if (p.type == "NameProperty") {
        return fname(a);
    } else if (p.type == "StructProperty" && (p.element_size == 12 || p.element_size == 24)) {
        double pitch = 0, yaw = 0, roll = 0;
        if (name_of(struct_type(p)) == "Rotator" && read_rotator(a, p.element_size, pitch, yaw, roll))
            std::snprintf(buffer, sizeof(buffer), "(P %.2f Y %.2f R %.2f)", pitch, yaw, roll);
    }
    return buffer;
}

bool write_number(Obj o, const char* name, double value) {
    const Prop p = prop_of(o, name);
    if (!p) return false;
    const uintptr_t a = o + p.offset;
    if (p.type == "FloatProperty") {
        const float v = float(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "DoubleProperty") return guarded_store(a, &value, sizeof(value));
    if (p.type == "IntProperty") {
        const int32_t v = int32_t(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "ByteProperty") {
        const uint8_t v = uint8_t(value);
        return guarded_store(a, &v, sizeof(v));
    }
    if (p.type == "BoolProperty") {
        uint8_t info[4] = {}, byte = 0;  // FieldSize, ByteOffset, ByteMask, FieldMask
        if (!guarded_copy(info, p.field + g_prop_size, 4) || !rd(a + info[1], byte)) return false;
        byte = value != 0 ? uint8_t(byte | info[3]) : uint8_t(byte & ~info[3]);
        return guarded_store(a + info[1], &byte, 1);
    }
    return false;
}

// ---------------------------------------------------------------- probe (snapshot diff)

namespace {

struct Leaf {
    std::string label;
    uintptr_t address;
    int kind;  // 0 float, 1 double, 2 rotator (float), 3 rotator (double)
    double base[3];
    Obj obj = 0;  // object the value lives in
};
std::vector<Leaf> g_leaves;

// Raw memory of each probed object (up to its class PropertiesSize), to find non-reflected C++ state.
struct RawObj {
    Obj o;
    std::string label;
    std::vector<uint8_t> bytes;
};
std::vector<RawObj> g_raw;
int g_snapshots = 0;  // snapshots this session (labels each diff in Gyro4All-probe.log)

// A rotation that followed the camera in a diff: a reflected rotator property, or a raw rotator
// (pitch, yaw) at an offset of a non-reflected object. Accumulated over every scored diff.
struct Candidate {
    std::string object;    // profile path ("Pawn.View")
    std::string cls;       // class name (informative)
    std::string property;  // reflected rotator ("" = raw)
    int32_t offset = -1;   // raw rotator offset (pitch, then yaw)
    int size = 0;          // 12 = float rotator, 24 = double rotator
    int hits = 0;          // scored diffs in which it followed the camera's pitch and yaw
    int exact = 0;         // ... of which it held the camera's exact values (else it moved by the same amount)
    std::string why_not;   // "" = usable in a profile, else why it is ignored
    bool world = false;    // a component's RelativeRotation with absolute rotation: the profile needs WorldRotation=1
    bool camera = false;   // ... of a CameraComponent (the engine takes the view from it)
};
std::map<std::string, Candidate> g_candidates;
int g_scored_diffs = 0;
struct Verdict {
    int result = kUntested;
    std::string note;
    bool pitch_followed = false, turns_pawn = false;
};
std::map<std::string, Verdict> g_verdicts;  // live test results by ProbeGroup id

int32_t properties_size(Obj cls) {  // UStruct::PropertiesSize follows ChildProperties (UE 4.25-5.x)
    int32_t n = 0;
    return rd(cls + g_child_props + 8, n) && n > 0 && n <= 0x20000 ? n : 0;
}

// Size of the engine-class part of o (e.g. "SceneComponent": its transform state), 0 if o isn't one.
int32_t engine_part_size(Obj o, const char* engine_class) {
    Obj c = class_of(o);
    for (int i = 0; c && i < 64; ++i, c = rdp(c + g_super))
        if (name_of(c) == engine_class) return properties_size(c);
    return 0;
}

int32_t scene_component_size(Obj o) { return engine_part_size(o, "SceneComponent"); }

bool near_angle(double v, double target) { return std::isfinite(v) && std::abs(wrap180(v - target)) < 0.6; }

bool read_leaf(const Leaf& l, double v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (l.kind == 0) { float f; if (!rd(l.address, f)) return false; v[0] = f; return true; }
    if (l.kind == 1) return rd(l.address, v[0]);
    return read_rotator(l.address, l.kind == 2 ? 12 : 24, v[0], v[1], v[2]);
}

// "Pawn.View(SHCharacterPlayViewComponent).Some.Prop" -> object "Pawn.View", class, property "Some.Prop"
void split_label(const std::string& label, std::string& object, std::string& cls, std::string& property) {
    const size_t open = label.find('('), close = label.find(')', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) {
        object = label, cls.clear(), property.clear();
        return;
    }
    object = label.substr(0, open);
    cls = label.substr(open + 1, close - open - 1);
    property = close + 2 <= label.size() ? label.substr(close + 2) : std::string{};
}

// Records one hit for a candidate (once per diff).
void score(const std::string& key, Candidate c, std::vector<std::string>& seen, bool exact) {
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    auto [it, inserted] = g_candidates.try_emplace(key, std::move(c));
    ++it->second.hits;
    if (exact) ++it->second.exact;
}

// A rotation (pitch p, yaw y; p0/y0 at the snapshot) followed the camera, whose POV is now `now` after turning by
// (dp, dy): it holds the POV's exact values, or it changed by the same amounts (a copy kept with an offset or a
// small lag: Hogwarts Legacy's camera stack keeps its pitch 3 degrees above the POV's). The scored turn is at least
// 10 degrees of yaw, so an unrelated value matching both changes is unlikely; the live test decides anyway.
bool follows(double p0, double y0, double p, double y, const double now[3], double dp, double dy, bool& exact) {
    exact = false;
    if (!std::isfinite(p) || !std::isfinite(y) || !std::isfinite(p0) || !std::isfinite(y0)) return false;
    if (std::abs(p) > 360 || std::abs(y) > 1e5 || near_angle(y0, y)) return false;  // not an angle, or didn't turn
    if (near_angle(y, now[1]) && near_angle(p, now[0])) return exact = true;  // pitch 350 = -10 (UE5 keeps 0..360)
    const double tol_y = (std::max)(0.6, std::abs(dy) * 0.03), tol_p = (std::max)(0.6, std::abs(dp) * 0.03);
    return std::abs(wrap180((y - y0) - dy)) < tol_y && std::abs(wrap180((p - p0) - dp)) < tol_p;
}

void collect_struct(uintptr_t base, Obj st, const std::string& prefix, int depth);

void collect_field(uintptr_t base, uintptr_t f, const std::string& prefix, int depth) {
    int32_t offset = -1, size = 0, dim = 1;
    rd(f + g_prop_offset, offset);
    rd(f + g_elem_size, size);
    rd(f + g_array_dim, dim);
    if (offset < 0 || dim != 1) return;
    const std::string type = field_type(f), label = prefix + field_name(f);
    const uintptr_t a = base + offset;
    if (type == "FloatProperty") g_leaves.push_back({label, a, 0, {}});
    else if (type == "DoubleProperty") g_leaves.push_back({label, a, 1, {}});
    else if (type == "StructProperty") {
        const Obj st = rdp(f + g_prop_size);
        const std::string sname = name_of(st);
        if (sname == "Rotator" && (size == 12 || size == 24)) g_leaves.push_back({label, a, size == 12 ? 2 : 3, {}});
        else if (depth < 3 && st && sname != "Vector" && sname != "Quat" && sname != "Transform" &&
                 sname != "LinearColor" && sname != "Color")
            collect_struct(a, st, label + ".", depth + 1);
    }
}

void collect_struct(uintptr_t base, Obj st, const std::string& prefix, int depth) {
    for (Obj c = st; c; c = rdp(c + g_super)) {
        int count = 0;
        for (uintptr_t f = rdp(c + g_child_props); f && count < 2000; f = rdp(f + g_field_next), ++count)
            collect_field(base, f, prefix, depth);
    }
}

// FRotator::Quaternion() (UE4 and UE5): x, y, z, w.
void rotator_quat(double pitch, double yaw, double roll, double q[4]) {
    const double k = 3.14159265358979323846 / 360.0;  // degrees to half-angle radians
    const double sp = std::sin(pitch * k), cp = std::cos(pitch * k);
    const double sy = std::sin(yaw * k), cy = std::cos(yaw * k);
    const double sr = std::sin(roll * k), cr = std::cos(roll * k);
    q[0] = cr * sp * sy - sr * cp * cy;
    q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

// A quaternion of floats (UE4) or doubles (UE5).
bool read_quat(uintptr_t address, bool wide, double q[4]) {
    if (wide) return guarded_copy(q, address, 4 * sizeof(double));
    float f[4];
    if (!guarded_copy(f, address, sizeof(f))) return false;
    for (int i = 0; i < 4; ++i) q[i] = f[i];
    return true;
}

bool write_quat(uintptr_t address, bool wide, const double q[4]) {
    if (wide) return guarded_store(address, q, 4 * sizeof(double));
    const float f[4] = {float(q[0]), float(q[1]), float(q[2]), float(q[3])};
    return guarded_store(address, f, sizeof(f));
}

bool same_rotation(const double a[4], const double b[4]) {  // q and -q are the same rotation
    double same = 0, opposite = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(a[i])) return false;
        same = (std::max)(same, std::abs(a[i] - b[i]));
        opposite = (std::max)(opposite, std::abs(a[i] + b[i]));
    }
    return (std::min)(same, opposite) < 2e-3;
}

// Offset of ComponentToWorld's rotation in a component class (found once per class, from a live component).
std::vector<std::pair<Obj, int32_t>> g_world_rotation_offsets;

// ComponentToWorld isn't reflected, so it's found by value: with absolute rotation its quaternion is
// RelativeRotation's. The rotation caches (FRotationConversionCache: the quaternion, then the rotator it came from)
// hold the same quaternion, so the transform is the match NOT followed by that rotator (it's followed by the world
// translation). Scanned at 16-byte alignment (FTransform is SIMD-aligned) within the SceneComponent part.
int32_t world_rotation_offset(Obj component, const Prop& relative) {
    const Obj cls = class_of(component);
    for (const auto& [c, offset] : g_world_rotation_offsets)
        if (c == cls) return offset;
    double p = 0, y = 0, r = 0;
    if (!read_rotator(component + relative.offset, relative.element_size, p, y, r)) return -1;
    double want[4];
    rotator_quat(p, y, r, want);
    const bool wide = relative.element_size == 24;
    const int32_t quat_size = wide ? 32 : 16, end = engine_part_size(component, "SceneComponent");
    for (int32_t off = 0x30; off + quat_size + relative.element_size <= end; off += 16) {
        double q[4];
        if (off == relative.offset || !read_quat(component + off, wide, q) || !same_rotation(q, want)) continue;
        double cp = 0, cy = 0, cr = 0;
        if (read_rotator(component + off + quat_size, relative.element_size, cp, cy, cr) && near_angle(cp, p) &&
            near_angle(cy, y) && near_angle(cr, r))
            continue;  // a rotation cache
        g_world_rotation_offsets.emplace_back(cls, off);
        log("ue: %s ComponentToWorld rotation at +0x%X (absolute rotation, %s)", name_of(cls).c_str(), off,
            wide ? "double" : "float");
        return off;
    }
    return -1;
}

}  // namespace

bool has_world_rotation(Obj component, const Prop& relative) {
    if (!g_ready || !component || !relative || (relative.element_size != 12 && relative.element_size != 24)) return false;
    const int32_t off = world_rotation_offset(component, relative);
    double p = 0, y = 0, r = 0, want[4], q[4];
    if (off < 0 || !read_rotator(component + relative.offset, relative.element_size, p, y, r)) return false;
    rotator_quat(p, y, r, want);
    return read_quat(component + off, relative.element_size == 24, q) && same_rotation(q, want);
}

bool add_world_rotation(Obj component, const Prop& relative, double pitch, double yaw, const char*& why) {
    why = nullptr;
    if (!g_ready || !component || !relative || (relative.element_size != 12 && relative.element_size != 24)) {
        why = "not a rotator property";
        return false;
    }
    const bool wide = relative.element_size == 24;
    const int32_t off = world_rotation_offset(component, relative);
    if (off < 0) {
        why = "its world rotation doesn't match RelativeRotation (no absolute rotation?)";
        return false;
    }
    double p = 0, y = 0, r = 0, q[4], want[4];
    if (!read_rotator(component + relative.offset, relative.element_size, p, y, r) || !read_quat(component + off, wide, q)) {
        why = "unreadable";
        return false;
    }
    rotator_quat(p, y, r, want);
    if (!same_rotation(q, want)) {  // the game is between its two writes, or the rotation stopped being absolute
        why = "its world rotation doesn't match RelativeRotation this frame";
        return false;
    }
    // Pitch stays short of straight up/down: past 90 the rotator flips (yaw + 180) when the game converts it back.
    double np = p + pitch;
    if (std::abs(np) > 89.0 && std::abs(np) > std::abs(p)) np = std::abs(p) > 89.0 ? p : std::copysign(89.0, np);
    const double ny = y + yaw;
    rotator_quat(np, ny, r, q);
    bool ok;
    if (wide) {
        const double v[3] = {np, ny, r};
        ok = guarded_store(component + relative.offset, v, sizeof(v));
    } else {
        const float v[3] = {float(np), float(ny), float(r)};
        ok = guarded_store(component + relative.offset, v, sizeof(v));
    }
    if (!ok || !write_quat(component + off, wide, q)) {
        why = "write failed";
        return false;
    }
    return true;
}

bool camera_pov(Obj camera, double& pitch, double& yaw) {
    if (!g_ready || !camera) return false;
    static Obj cached_class = 0;
    static int32_t offset = -1, size = 0;
    const Obj cls = class_of(camera);
    if (cls != cached_class) {
        cached_class = cls;
        offset = -1;
        for (const char* cache : {"CameraCachePrivate", "CameraCache"}) {
            const Prop c = find_prop(cls, cache);
            const Prop pov = c ? find_prop(struct_type(c), "POV") : Prop{};
            const Prop rot = pov ? find_prop(struct_type(pov), "Rotation") : Prop{};
            if (rot && (rot.element_size == 12 || rot.element_size == 24)) {
                offset = c.offset + pov.offset + rot.offset;
                size = rot.element_size;
                break;
            }
        }
    }
    double roll = 0;
    return offset >= 0 && read_rotator(camera + offset, size, pitch, yaw, roll) && std::isfinite(pitch) &&
           std::isfinite(yaw);
}

bool camera_pov_location(Obj camera, double location[3]) {
    if (!g_ready || !camera) return false;
    static Obj cached_class = 0;
    static int32_t offset = -1, size = 0;
    const Obj cls = class_of(camera);
    if (cls != cached_class) {
        cached_class = cls;
        offset = -1;
        for (const char* cache : {"CameraCachePrivate", "CameraCache"}) {
            const Prop c = find_prop(cls, cache);
            const Prop pov = c ? find_prop(struct_type(c), "POV") : Prop{};
            const Prop loc = pov ? find_prop(struct_type(pov), "Location") : Prop{};
            if (loc && (loc.element_size == 12 || loc.element_size == 24)) {
                offset = c.offset + pov.offset + loc.offset;
                size = loc.element_size;
                break;
            }
        }
    }
    if (offset < 0) return false;
    for (int i = 0; i < 3; ++i) {
        if (size == 24) {
            if (!rd(camera + offset + i * 8, location[i])) return false;
        } else {
            float f = 0;
            if (!rd(camera + offset + i * 4, f)) return false;
            location[i] = f;
        }
        if (!std::isfinite(location[i])) return false;
    }
    return true;
}

size_t probe_snapshot(const std::vector<std::pair<std::string, Obj>>& roots) {
    g_leaves.clear();
    if (!g_ready) return 0;
    // Each object is labelled with its profile path, e.g. "Pawn.View(SHCharacterPlayViewComponent)":
    // the part before "(" can be used as Object= in a profile view.
    struct Found {
        Obj o, root;
        std::string role;
    };
    std::vector<Found> found;
    // First object (by index) under each outer whose class chain has each class name: what "@Class" resolves to.
    std::map<std::pair<Obj, std::string>, Obj> first_of;
    const int32_t n = object_count();
    for (int32_t i = 0; i < n; ++i) {
        const Obj o = object_at(i);
        if (!o || is_cdo(o)) continue;
        std::string role;
        Obj x = o;
        for (int d = 0; x && d < 8 && role.empty(); ++d) {
            for (const auto& [r_name, r] : roots)
                if (r && x == r && role.empty()) role = r_name;
            if (role.empty()) x = rdp(x + g_obj_outer);
        }
        if (role.empty()) continue;
        found.push_back({o, x, role});
        if (o == x) continue;
        Obj c = class_of(o);
        for (int depth = 0; c && depth < 64; ++depth, c = rdp(c + g_super)) first_of.try_emplace({rdp(o + g_obj_outer), name_of(c)}, o);
    }
    // A path step: the object's name, or "@Class" when the name is generated ("<Class>_<number>", a new number every
    // session, e.g. Hogwarts Legacy's CameraStackBehaviorConvertToTranslation_2147460121) and "@Class" finds it.
    auto step = [&](Obj y) {
        const std::string name = name_of(y), cls = name_of(class_of(y));
        const bool generated = name.size() > cls.size() + 1 && name.compare(0, cls.size(), cls) == 0 &&
                               name[cls.size()] == '_' &&
                               name.find_first_not_of("0123456789", cls.size() + 1) == std::string::npos;
        if (generated) {
            const auto it = first_of.find({rdp(y + g_obj_outer), cls});
            if (it != first_of.end() && it->second == y) return "@" + cls;
        }
        return name;
    };
    std::vector<std::pair<Obj, std::string>> objects;
    for (const Found& f : found) {
        std::string path = f.role;
        for (Obj y = f.o; y && y != f.root; y = rdp(y + g_obj_outer)) {
            const size_t root_len = path.find('.') == std::string::npos ? path.size() : path.find('.');
            path.insert(root_len, "." + step(y));
        }
        objects.emplace_back(f.o, path + "(" + name_of(class_of(f.o)) + ")");
    }
    for (const auto& [o, label] : objects) {
        const std::string prefix = label + ".";
        const size_t first = g_leaves.size();
        Obj c = class_of(o);
        for (int depth = 0; c && depth < 64; ++depth) {
            const std::string cname = name_of(c);
            if (cname == "Object") break;
            int count = 0;
            for (uintptr_t f = rdp(c + g_child_props); f && count < 4000; f = rdp(f + g_field_next), ++count)
                collect_field(o, f, prefix, 0);
            c = rdp(c + g_super);
        }
        for (size_t i = first; i < g_leaves.size(); ++i) g_leaves[i].obj = o;
    }
    for (Leaf& l : g_leaves) read_leaf(l, l.base);
    g_raw.clear();
    for (const auto& [o, label] : objects) {
        const int32_t size = properties_size(class_of(o));
        RawObj r{o, label, std::vector<uint8_t>(size_t(size))};
        if (size && guarded_copy(r.bytes.data(), o, r.bytes.size())) g_raw.push_back(std::move(r));
    }
    ++g_snapshots;
    probe_log("probe: snapshot #%d of %zu objects, %zu values", g_snapshots, objects.size(), g_leaves.size());
    log("probe: snapshot #%d taken (%zu objects, %zu values) -> Gyro4All-probe.log", g_snapshots, objects.size(),
        g_leaves.size());
    return g_leaves.size();
}

int probe_diff(bool verbose) {
    if (g_leaves.empty()) {
        if (g_snapshots == 0) {
            log("probe: F8 pressed but there is no snapshot yet: press F7 first");
            probe_log("probe: no snapshot (press F7 first)");
        } else {
            log("probe: snapshot #%d holds no values, nothing to compare (the engine's property layout wasn't read; "
                "see the 'ue: layout' line)", g_snapshots);
            probe_log("probe: snapshot #%d holds no values, nothing to compare", g_snapshots);
        }
        return 0;
    }
    probe_log("probe: %s diff against snapshot #%d", verbose ? "manual" : "automatic", g_snapshots);
    int rotators = 0, scalars = 0;
    if (verbose) {
        for (int pass = 0; pass < 2; ++pass) {
            for (const Leaf& l : g_leaves) {
                const bool rot = l.kind >= 2;
                if (rot != (pass == 0)) continue;
                double v[3];
                if (!read_leaf(l, v)) continue;
                double change = 0;
                for (int k = 0; k < 3; ++k) change = (std::max)(change, std::abs(v[k] - l.base[k]));
                if (rot) {
                    if (change < 0.5 || ++rotators > 150) continue;
                    probe_log("  rot %s (P %.2f Y %.2f R %.2f) -> (P %.2f Y %.2f R %.2f)", l.label.c_str(), l.base[0],
                              l.base[1], l.base[2], v[0], v[1], v[2]);
                } else {
                    if (change < 0.001 || ++scalars > 250) continue;
                    probe_log("  num %s %.4f -> %.4f", l.label.c_str(), l.base[0], v[0]);
                }
            }
        }
        probe_log("probe: diff done (%d rotators, %d numbers changed)", rotators, scalars);
    }

    // The camera's final view rotation, before and now (CameraCache in older UE 4, e.g. Days Gone's 4.11, as in
    // camera_pov).
    const Leaf* pov = nullptr;
    for (const Leaf& l : g_leaves)
        if (l.kind >= 2 && l.label.size() > 28 &&
            (l.label.ends_with("CameraCachePrivate.POV.Rotation") || l.label.ends_with(".CameraCache.POV.Rotation")))
            pov = &l;
    double now[3] = {};
    if (!pov || !read_leaf(*pov, now)) {
        log("probe: diff of snapshot #%d (%d rotators, %d numbers changed; no camera POV, raw diff skipped) -> "
            "Gyro4All-probe.log", g_snapshots, rotators, scalars);
        probe_log("probe: no camera POV rotation, raw diff skipped");
        return 0;
    }
    const double dp = wrap180(now[0] - pov->base[0]), dy = wrap180(now[1] - pov->base[1]);
    probe_log("probe: raw diff, POV pitch %.2f -> %.2f (d %.2f), yaw %.2f -> %.2f (d %.2f)", pov->base[0], now[0], dp,
              pov->base[1], now[1], dy);
    const double kRad = 57.29577951308232;
    int matches = 0;
    if (verbose) {
        for (const RawObj& r : g_raw) {
            std::vector<uint8_t> cur(r.bytes.size());
            if (!guarded_copy(cur.data(), r.o, cur.size())) continue;
            for (size_t off = 0x28; off + 4 <= cur.size() && matches < 300; off += 4) {
                for (int wide = 0; wide < 2; ++wide) {
                    if (wide && (off % 8 != 0 || off + 8 > cur.size())) continue;
                    double a = 0, b = 0;
                    if (wide) {
                        std::memcpy(&a, &r.bytes[off], 8);
                        std::memcpy(&b, &cur[off], 8);
                    } else {
                        float fa, fb;
                        std::memcpy(&fa, &r.bytes[off], 4);
                        std::memcpy(&fb, &cur[off], 4);
                        a = fa, b = fb;
                    }
                    if (!std::isfinite(a) || !std::isfinite(b) || a == b || std::abs(b) > 1e5) continue;
                    const char* what = nullptr;
                    if (near_angle(b, now[1]) && std::abs(dy) > 2) what = "= POV yaw";
                    else if (near_angle(b, now[0]) && std::abs(dp) > 2) what = "= POV pitch";
                    else if (std::abs(dy) > 2 && near_angle(b - a, dy)) what = "d = POV d yaw";
                    else if (std::abs(dp) > 2 && near_angle(b - a, dp)) what = "d = POV d pitch";
                    else if (std::abs(dy) > 2 && near_angle(b * kRad, now[1])) what = "= POV yaw (rad)";
                    else if (std::abs(dp) > 2 && near_angle(b * kRad, now[0])) what = "= POV pitch (rad)";
                    if (!what) continue;
                    ++matches;
                    probe_log("  raw %s+0x%zX %s %.4f -> %.4f  %s", r.label.c_str(), off, wide ? "double" : "float", a, b,
                              what);
                }
            }
        }
        probe_log("probe: raw diff done (%d matches)", matches);
    }

    // Scoring for the suggested profile: only turns large enough that matching the camera's pitch and
    // yaw at the same time can't be a coincidence.
    if (std::abs(dy) < 10) {
        probe_log("probe: turn too small to score (yaw %.1f, needs more than 10 degrees)", dy);
        log("probe: diff of snapshot #%d (%d rotators, %d numbers, %d raw matches; not scored, yaw turn %.0f < 10) -> "
            "Gyro4All-probe.log", g_snapshots, rotators, scalars, matches, std::abs(dy));
        return 0;
    }
    ++g_scored_diffs;
    std::vector<std::string> seen;
    std::vector<uintptr_t> reflected;  // addresses of reflected rotators (the raw scan skips them)
    int found = 0;
    for (const Leaf& l : g_leaves) {
        if (l.kind < 2) continue;
        reflected.push_back(l.address);
        double v[3];
        if (&l == pov || !read_leaf(l, v)) continue;
        bool exact = false;
        if (!follows(l.base[0], l.base[1], v[0], v[1], now, dp, dy, exact)) continue;
        Candidate c;
        split_label(l.label, c.object, c.cls, c.property);
        c.size = l.kind == 2 ? 12 : 24;
        // The camera manager is mostly output (POV caches), but a game's own subclass may keep its look state past the
        // engine's PlayerCameraManager part, also in a struct (Jedi: Fallen Order's RsCameraManager ModeOutputs.Rotation,
        // usable as Property=ModeOutputs.Rotation), as for raw offsets below.
        const int32_t camera_engine = c.object == "Camera" ? engine_part_size(l.obj, "PlayerCameraManager") : 0;
        if (c.object.rfind("Camera", 0) == 0 && !(camera_engine > 0 && l.address >= l.obj + camera_engine))
            c.why_not = "camera output";
        else if (c.property == "RelativeRotation" && read_obj(l.obj, "AttachParent") &&
                 has_world_rotation(l.obj, prop_of(l.obj, "RelativeRotation"))) {
            // An attached component with absolute rotation: the game sets its world rotation itself, so it may be the
            // state it turns (Severed Steel's FirstPersonCamera). Root components are the actor's rotation (output).
            c.world = true;
            c.camera = is_a(l.obj, "CameraComponent");
        } else if (c.property.find("RelativeRotation") != std::string::npos) c.why_not = "component transform (output)";
        else if (c.object == "PC" && c.property == "ControlRotation") c.why_not = "the default view (already tried)";
        score(c.object + "|" + c.property, c, seen, exact);
        ++found;
        if (!verbose)
            probe_log("  candidate %s (%s rotator%s)", l.label.c_str(), l.kind == 2 ? "float" : "double",
                      exact ? "" : ", moved by the same amount");
    }
    for (const RawObj& r : g_raw) {
        std::vector<uint8_t> cur(r.bytes.size());
        if (!guarded_copy(cur.data(), r.o, cur.size())) continue;
        const int32_t scene = scene_component_size(r.o);
        // The camera manager is mostly output (POV caches), but a game's own subclass may keep its look state past
        // the engine's PlayerCameraManager part (Life is Strange: LiSPlayerCameraManager +0x33CC/+0x33FC).
        const bool camera_root = r.label.rfind("Camera(", 0) == 0;
        const int32_t camera_engine = camera_root ? engine_part_size(r.o, "PlayerCameraManager") : 0;
        for (int wide = 0; wide < 2; ++wide) {
            const size_t elem = wide ? 8 : 4;
            for (size_t off = 0x28; off + 2 * elem <= cur.size(); off += elem) {
                double p = 0, y = 0, p0 = 0, y0 = 0;
                if (wide) {
                    std::memcpy(&p, &cur[off], 8);
                    std::memcpy(&y, &cur[off + 8], 8);
                    std::memcpy(&p0, &r.bytes[off], 8);
                    std::memcpy(&y0, &r.bytes[off + 8], 8);
                } else {
                    float fp, fy, fp0, fy0;
                    std::memcpy(&fp, &cur[off], 4);
                    std::memcpy(&fy, &cur[off + 4], 4);
                    std::memcpy(&fp0, &r.bytes[off], 4);
                    std::memcpy(&fy0, &r.bytes[off + 4], 4);
                    p = fp, y = fy, p0 = fp0, y0 = fy0;
                }
                bool exact = false;
                if (!follows(p0, y0, p, y, now, dp, dy, exact)) continue;
                if (std::find(reflected.begin(), reflected.end(), r.o + off) != reflected.end()) continue;
                Candidate c;
                std::string ignored;
                split_label(r.label, c.object, c.cls, ignored);
                c.offset = int32_t(off);
                c.size = wide ? 24 : 12;
                if (c.object.rfind("Camera", 0) == 0 && !(camera_root && camera_engine > 0 && int32_t(off) >= camera_engine))
                    c.why_not = "camera output";
                else if (scene > 0 && int32_t(off) < scene) c.why_not = "component transform (output)";
                char key[32];
                std::snprintf(key, sizeof key, "|+0x%X|%d", c.offset, c.size);
                score(c.object + key, c, seen, exact);
                ++found;
                if (!verbose)
                    probe_log("  candidate %s+0x%zX (%s rotator%s)", r.label.c_str(), off, wide ? "double" : "float",
                              exact ? "" : ", moved by the same amount");
            }
        }
    }
    probe_log("probe: %d rotations followed the camera (scored diff #%d)", found, g_scored_diffs);
    log("probe: diff of snapshot #%d (%d rotators, %d numbers, %d raw matches, %d candidates, scored #%d) -> "
        "Gyro4All-probe.log", g_snapshots, rotators, scalars, matches, found, g_scored_diffs);
    return g_scored_diffs;
}

std::vector<ProbeGroup> probe_groups() {
    std::vector<ProbeGroup> groups;
    if (g_scored_diffs < 2) return groups;
    // Followed the camera in most scored diffs, at least two. Not all: a look rotation kept with an offset that depends
    // on the pitch (Hogwarts Legacy's framing) can miss a capture with a large pitch change. The live test decides.
    const int needed = (std::max)(2, (g_scored_diffs + 1) / 2);
    std::vector<int> rank;
    for (const auto& [key, c] : g_candidates) {
        if (c.hits < needed) continue;
        // One group per object for raw rotators of one size (a profile writes all its offsets, like Cronos' two
        // copies), one per reflected property. A raw group is usable if any of its rotators is.
        auto it = std::find_if(groups.begin(), groups.end(), [&](const ProbeGroup& g) {
            return c.property.empty() && g.property.empty() && g.object == c.object && g.size == c.size;
        });
        if (it == groups.end()) {
            ProbeGroup g;
            g.object = c.object, g.cls = c.cls, g.property = c.property, g.size = c.size, g.world = c.world;
            g.why_not = c.why_not;
            g.id = c.object + "|" + (c.property.empty() ? "raw" + std::to_string(c.size) : c.property);
            groups.push_back(g);
            // Inputs usually live in the pawn or its components; then the view target, then the controller and the
            // camera manager's own fields. A camera component with absolute rotation comes first: the engine takes
            // the view from it, and only the game sets its rotation.
            rank.push_back(c.camera ? 4 : c.object.rfind("Pawn", 0) == 0 ? 3 : c.object.rfind("ViewTarget", 0) == 0 ? 2 : 1);
            it = groups.end() - 1;
        } else if (c.why_not.empty()) {
            it->why_not.clear();
        }
        if (c.property.empty()) it->offsets.push_back(c.offset);
        it->hits = (std::max)(it->hits, c.hits);
        it->exact = (std::max)(it->exact, c.exact);
    }
    for (ProbeGroup& g : groups) {
        std::sort(g.offsets.begin(), g.offsets.end());
        if (auto v = g_verdicts.find(g.id); v != g_verdicts.end()) {
            g.verdict = v->second.result, g.note = v->second.note;
            g.pitch_followed = v->second.pitch_followed, g.turns_pawn = v->second.turns_pawn;
        }
    }
    // Among the rotations that moved the camera in the live test: one that turns pitch too first (a yaw-only one is
    // learned with PitchView=1), then one that also turns the character. That one is upstream of both, the look input
    // they read; one that turns the camera alone can be a camera stage the game rebuilds from it in other states
    // (Silent Hill 2: the spring arm turns the camera standing still but is rewritten while walking, and while aiming
    // only moves the crosshair; the View component turns the camera and James in all three).
    auto pick_rank = [](const ProbeGroup& g) {
        return g.verdict != kMovesCamera ? 0 : 1 + (g.pitch_followed ? 2 : 0) + (g.turns_pawn ? 1 : 0);
    };
    std::vector<size_t> order(groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const ProbeGroup &ga = groups[a], &gb = groups[b];
        if (pick_rank(ga) != pick_rank(gb)) return pick_rank(ga) > pick_rank(gb);
        if (ga.why_not.empty() != gb.why_not.empty()) return ga.why_not.empty();
        if (rank[a] != rank[b]) return rank[a] > rank[b];
        if (ga.offsets.size() != gb.offsets.size()) return ga.offsets.size() > gb.offsets.size();
        return ga.property.empty() < gb.property.empty();  // reflected (named) before raw at equal rank
    });
    std::vector<ProbeGroup> sorted;
    for (size_t i : order) sorted.push_back(groups[i]);
    return sorted;
}

void probe_verdict(const std::string& id, int verdict, const std::string& note, bool pitch_followed, bool turns_pawn) {
    g_verdicts[id] = {verdict, note, pitch_followed, turns_pawn};
}

void probe_reset() {
    g_leaves.clear();
    g_raw.clear();
    g_candidates.clear();
    g_verdicts.clear();
    g_scored_diffs = 0;
}

std::string probe_suggestion(const std::string& exe_stem, std::string& summary, bool as_profile, bool& verified) {
    summary.clear();
    verified = false;
    if (g_scored_diffs < 2) return {};
    const std::vector<ProbeGroup> groups = probe_groups();
    // The pick: a group that moved the camera in the live test (the best of them comes first, see probe_groups),
    // else the passive best guess (usable groups the live test didn't reject).
    const ProbeGroup* best = nullptr;
    for (const ProbeGroup& g : groups)
        if (g.verdict == kMovesCamera) {
            best = &g;
            verified = true;
            break;
        }
    if (!best)
        for (const ProbeGroup& g : groups)
            if (g.why_not.empty() && g.verdict == kUntested) {
                best = &g;
                break;
            }
    auto view_lines = [](const ProbeGroup& g, const char* indent) {
        std::string s = std::string(indent) + "Object=" + g.object + "\n";
        if (!g.property.empty())
            return s + indent + "Property=" + g.property + "\n" + (g.world ? std::string(indent) + "WorldRotation=1\n" : "");
        std::string offsets;
        for (int32_t off : g.offsets) offsets += (offsets.empty() ? "" : ",") + std::format("0x{:X}", off);
        return s + indent + "Offsets=" + offsets + "\n" + indent + "RotatorSize=" + std::to_string(g.size) + "\n";
    };
    auto result = [](const ProbeGroup& g) -> std::string {
        switch (g.verdict) {
        case kMovesCamera: return "moved the camera in the live test" + (g.note.empty() ? "" : " (" + g.note + ")");
        case kOverwritten: return "live test: the game overwrote it (an output)";
        case kNoEffect: return "live test: kept but the camera didn't move";
        case kUntestable: return "live test: " + (g.note.empty() ? std::string("not testable") : g.note);
        case kInconsistent: return "live test: moved the camera once, not the second time" + (g.note.empty() ? "" : " (" + g.note + ")");
        case kTurnsInPlace: return "live test: turned the view in place, the camera didn't orbit like with the stick" +
                                   (g.note.empty() ? "" : " (" + g.note + ")");
        default: return g.why_not.empty() ? "not tested live" : g.why_not;
        }
    };

    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string when = std::format("{:04}-{:02}-{:02} {:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::string text =
        as_profile
            ? std::format("; Gyro4All {} profile for {}, created by the camera probe ({} scored captures, {}).\n", GYRO4ALL_VERSION,
                          exe_stem, g_scored_diffs, when)
            : std::format("; Gyro4All {} probe suggestion for {} ({} scored captures, {}).\n", GYRO4ALL_VERSION, exe_stem,
                          g_scored_diffs, when);
    if (verified)
        text += "; VERIFIED LIVE: the probe wrote small turns into this rotation and the camera followed them, twice.\n"
                "; Gyro4All set Probe=0 in Gyro4All\\Gyro4All.ini when it wrote this: restart the game to use it.\n";
    else
        text += "; NOT VERIFIED: no rotation moved the camera in the live test (or it didn't run). This is only the\n"
                "; best guess from the captures; it may well not work.\n";
    if (!as_profile)
        text += std::format("; To try it: rename this file to {}.ini (replacing the profile there, if any) and restart the game.\n",
                            exe_stem);
    text += "\n";
    if (!best) {
        text += "; No usable rotation followed the camera in every capture. The camera may be driven by state the probe\n"
                "; can't see (quaternions, input accumulators, animation). See ADVANCED.md, \"Finding the camera of a new game\".\n";
        summary = "no usable candidate";
    } else {
        text += "[View.Default]\nLabel=Exploration\n" + view_lines(*best, "");
        summary = best->object + (best->property.empty() ? std::format(" raw x{}", best->offsets.size()) : "." + best->property) +
                  (best->world ? " (WorldRotation)" : "") + (verified ? ", verified live" : ", not verified");
    }
    // Everything else that followed the camera, with what the live test (or the passive rules) said about it.
    std::string others;
    int listed = 0;
    for (const ProbeGroup& g : groups) {
        if (&g == best || ++listed > 16) continue;
        others += "  ; " + g.object + "(" + g.cls + ")" +
                  (g.property.empty() ? " raw " + [&] {
                      std::string o;
                      for (int32_t off : g.offsets) o += (o.empty() ? "" : ",") + std::format("0x{:X}", off);
                      return o;
                  }()
                                      : "." + g.property) +
                  ": " + result(g) + "\n";
    }
    if (!others.empty()) text += "\n; Also followed the camera:\n" + others;
    return text;
}

bool read_ptr(uintptr_t address, uintptr_t& out) {
    out = rdp(address);
    return out != 0;
}

bool read_i32(uintptr_t address, int32_t& out) { return rd(address, out); }

}  // namespace gyro4all::ue
