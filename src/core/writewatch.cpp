#include "writewatch.hpp"

#include "dinputpad.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>

namespace gyro4all::writewatch {

namespace {

// One writing instruction. The exception handler claims a free slot for a new instruction address (rip) and fills
// the rest once; the watcher thread logs it when ready is set. No locks or allocations in the handler: it runs on the
// game's threads, inside whatever they were doing.
struct Writer {
    std::atomic<uint64_t> rip{0};
    std::atomic<uint32_t> count{0};
    std::atomic<bool> ready{false};
    bool logged = false;  // watcher thread only
    DWORD thread = 0;
    uint64_t regs[17] = {};  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15 (CONTEXT order), then rflags
    float xmm[4] = {};       // low float of xmm0..xmm3
    uint32_t value = 0;      // the watched 4 bytes after the write
    uint64_t returns[8] = {};  // exe addresses on the stack (heuristic: may hold stale values)
    // Callers by unwinding (x64 unwind data): return address, then rsp rbx rbp rsi rdi r12 r13 r14 r15 in that frame
    // (non-volatile, so the caller's own values). Stops at code without unwind data.
    uint64_t frames[6][10] = {};
};
constexpr int kWriters = 64;
Writer g_writers[kWriters];
// Hits after which the watch disarms itself: each hit stops a game thread for an exception (a few microseconds), so a
// breakpoint on hot memory (e.g. a stack slot every function reuses) would stall the game.
constexpr uint64_t kMaxHits = 100000;
// WriteWatch=<address>: writes; r:<address>: reads and writes (x86 has no read-only breakpoint); x:<address>: an
// execute breakpoint (a function's entry).
enum class Mode { write, access, execute };
std::atomic<uintptr_t> g_target{0};  // the watched address (4-byte aligned for data), 0 = disarmed
std::atomic<uintptr_t> g_previous{0};  // the address before a named one moved: threads still on it are moved over

// Engine-provided names for addresses (provide()).
struct Provider {
    std::atomic<AddressFn> fn{nullptr};
    char name[16] = {};
};
Provider g_providers[4];
std::atomic<bool> g_exec{false};      // execute mode

// Execute mode: rcx (a method's this) and the floats at WriteWatchFields offsets from it, on every call.
struct Sample {
    int64_t time = 0;  // QueryPerformanceCounter
    uint64_t rcx = 0;
    DWORD thread = 0;
    float values[6] = {};
};
constexpr uint32_t kSamples = 8192;
Sample g_samples[kSamples];
std::atomic<uint32_t> g_sample_count{0};
int g_field_count = 0;
int32_t g_fields[6] = {};
std::atomic<uint64_t> g_hits{0}, g_dropped{0};
uintptr_t g_image_begin = 0, g_image_end = 0;

// Unwinds a copy of the faulting context into frames (POD only, so the walk can be guarded: protected code may have
// no or odd unwind data, and a wrong step could read a bad stack slot).
void unwind(const CONTEXT& c, uint64_t (&frames)[6][10]) {
    CONTEXT u = c;
    __try {
        for (int f = 0; f < 6; ++f) {
            DWORD64 base = 0;
            if (PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &base, nullptr)) {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, fn, &u, &handler_data, &establisher, nullptr);
            } else {
                u.Rip = *reinterpret_cast<const DWORD64*>(u.Rsp);  // leaf function: the return address is on top
                u.Rsp += 8;
            }
            if (u.Rip < 0x10000) return;
            const DWORD64 v[10] = {u.Rip, u.Rsp, u.Rbx, u.Rbp, u.Rsi, u.Rdi, u.R12, u.R13, u.R14, u.R15};
            std::memcpy(frames[f], v, sizeof v);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void record(const CONTEXT& c, uint64_t rip) {
    g_hits.fetch_add(1, std::memory_order_relaxed);
    for (Writer& w : g_writers) {
        uint64_t cur = w.rip.load(std::memory_order_acquire);
        if (cur == rip) {
            w.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (cur == 0 && w.rip.compare_exchange_strong(cur, rip)) {
            w.thread = GetCurrentThreadId();
            const DWORD64* r = &c.Rax;  // Rax..R15 are contiguous in CONTEXT
            for (int i = 0; i < 16; ++i) w.regs[i] = r[i];
            w.regs[16] = c.EFlags;
            for (int i = 0; i < 4; ++i) std::memcpy(&w.xmm[i], &(&c.Xmm0)[i], 4);
            w.value = *reinterpret_cast<const volatile uint32_t*>(g_target.load(std::memory_order_relaxed));
            // Return addresses into the exe on the first 64 stack slots (the stack of this very thread: readable).
            int n = 0;
            const auto* stack = reinterpret_cast<const uint64_t*>(c.Rsp);
            for (int i = 0; i < 64 && n < 8; ++i)
                if (stack[i] >= g_image_begin && stack[i] < g_image_end) w.returns[n++] = stack[i];
            unwind(c, w.frames);
            w.count.fetch_add(1, std::memory_order_relaxed);
            w.ready.store(true, std::memory_order_release);
            return;
        }
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& c = *info->ContextRecord;
    const uintptr_t target = g_target.load(std::memory_order_relaxed);
    // Ours: DR0 fired on the watched address. A data breakpoint traps after the instruction, so ExceptionAddress is
    // the next instruction. Also after disarming, while a thread still has the old DR0 set: swallowed, never passed on,
    // and that thread lets go of DR0 right away (as it does once the watch has had kMaxHits hits; the watcher thread
    // clears the other threads).
    const uintptr_t previous = g_previous.load(std::memory_order_relaxed);
    if (!(c.Dr6 & 1) || !c.Dr0 || (target && c.Dr0 != target && c.Dr0 != previous)) return EXCEPTION_CONTINUE_SEARCH;
    // A thread still watching where a named address was: it watches the new place from now on.
    if (target && c.Dr0 == previous && previous != target) {
        c.Dr0 = target;
        c.Dr6 = 0;
        if (!(c.Dr7 & (DWORD64(3) << 16))) c.EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const bool exec = !(c.Dr7 & (DWORD64(3) << 16));  // R/W0 = 00
    const bool armed = target && g_hits.load(std::memory_order_relaxed) < kMaxHits;
    if (armed) {
        record(c, reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress));
    } else {
        c.Dr7 &= ~(DWORD64(3) | (DWORD64(0xF) << 16));
        c.Dr0 = 0;
    }
    if (armed && exec) {
        const uint32_t i = g_sample_count.fetch_add(1, std::memory_order_relaxed);
        if (i < kSamples) {
            Sample& s = g_samples[i];
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            s.time = now.QuadPart;
            s.rcx = c.Rcx;
            s.thread = GetCurrentThreadId();
            for (int f = 0; f < g_field_count; ++f) mem::guarded_copy(&s.values[f], c.Rcx + g_fields[f], 4);
        }
    }
    // An execute breakpoint (R/W0 = 00) is a fault, raised before the instruction: set the resume flag so it runs
    // once without trapping again.
    if (exec) c.EFlags |= 0x10000;
    c.Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (target != 0) or clears DR0 as a 4-byte write or read/write breakpoint, or an execute one, on every thread of
// the game but this one.
int set_breakpoint(uintptr_t target, Mode mode = Mode::write) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    int done = 0;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                // DR7: L0 (bit 0), R/W0 (bits 16-17) = 01 write, 11 read or write, 00 execute; LEN0 (bits 18-19) =
                // 11 four bytes, 00 for execute.
                c.Dr7 &= ~(DWORD64(3) | (DWORD64(0xF) << 16));
                c.Dr0 = target;
                const DWORD64 rw = mode == Mode::execute ? 0 : mode == Mode::access ? 3 : 1;
                if (target) c.Dr7 |= 1 | rw << 16 | (rw ? DWORD64(3) << 18 : 0);
                if (SetThreadContext(t, &c)) ++done;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return done;
}

std::string hex_bytes(uintptr_t from, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = 0;
        char s[4];
        if (mem::guarded_copy(&b, from + i, 1)) std::snprintf(s, sizeof s, "%02X", b);
        else std::snprintf(s, sizeof s, "??");
        out += s;
    }
    return out;
}

void log_writer(Writer& w) {
    const uint64_t rip = w.rip.load();
    float value = 0;
    std::memcpy(&value, &w.value, 4);
    log("writewatch: writer %s (thread %lu): value now %.6f (0x%08X); xmm0..3 low %.6f %.6f %.6f %.6f",
        mem::module_offset(rip).c_str(), w.thread, value, w.value, w.xmm[0], w.xmm[1], w.xmm[2], w.xmm[3]);
    static const char* const names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8",
                                        "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rflags"};
    std::string line;
    for (int i = 0; i < 17; ++i) {
        char s[40];
        std::snprintf(s, sizeof s, "%s%s=%llX", i ? " " : "", names[i], (unsigned long long)w.regs[i]);
        line += s;
    }
    log("writewatch:   %s", line.c_str());
    line.clear();
    for (uint64_t r : w.returns)
        if (r) line += " " + mem::module_offset(r);
    log("writewatch:   stack scan (heuristic):%s", line.empty() ? " none in the exe" : line.c_str());
    for (int f = 0; f < 6 && w.frames[f][0]; ++f) {
        const uint64_t* v = w.frames[f];
        log("writewatch:   caller #%d %s rsp=%llX rbx=%llX rbp=%llX rsi=%llX rdi=%llX r12=%llX r13=%llX r14=%llX "
            "r15=%llX",
            f + 1, mem::module_offset(v[0]).c_str(), (unsigned long long)v[1], (unsigned long long)v[2],
            (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
            (unsigned long long)v[7], (unsigned long long)v[8], (unsigned long long)v[9]);
    }
    if (!w.frames[0][0]) log("writewatch:   no caller (no unwind data at the writer)");
    // The writing instruction ends at rip: 48 bytes before it, then 16 from it (decode offline).
    log("writewatch:   code %s+0x%X..: %s | %s", mem::module_offset(rip - 48).c_str(), 48u,
        hex_bytes(rip - 48, 48).c_str(), hex_bytes(rip, 16).c_str());
    w.logged = true;
}

void log_new_writers() {
    for (Writer& w : g_writers)
        if (!w.logged && w.ready.load(std::memory_order_acquire)) log_writer(w);
}

// Execute mode: the objects the method ran on, then the sampled fields every 200 ms (first call in each window).
void sample_summary() {
    const uint32_t n = std::min(g_sample_count.load(), kSamples);
    if (!n) return;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    uint64_t seen[16] = {};
    uint32_t counts[16] = {};
    for (uint32_t i = 0; i < n; ++i)
        for (int k = 0; k < 16; ++k)
            if (!seen[k] || seen[k] == g_samples[i].rcx) {
                seen[k] = g_samples[i].rcx;
                ++counts[k];
                break;
            }
    for (int k = 0; k < 16 && seen[k]; ++k)
        log("writewatch:   this 0x%llX: %u calls", (unsigned long long)seen[k], counts[k]);
    if (!g_field_count) return;
    const int64_t start = g_samples[0].time, window = freq.QuadPart / 5;
    int64_t next = start;
    for (uint32_t i = 0; i < n; ++i) {
        const Sample& s = g_samples[i];
        if (s.time < next) continue;
        next = s.time + window;
        std::string line;
        for (int f = 0; f < g_field_count; ++f) {
            char v[40];
            std::snprintf(v, sizeof v, " +0x%X=%.5f", g_fields[f], s.values[f]);
            line += v;
        }
        log("writewatch:   t=%6.2fs this=0x%llX%s", double(s.time - start) / double(freq.QuadPart),
            (unsigned long long)s.rcx, line.c_str());
    }
}

void summary(const char* why) {
    log("writewatch: %s; %llu hits, %llu from instructions beyond the first %d", why,
        (unsigned long long)g_hits.load(), (unsigned long long)g_dropped.load(), kWriters);
    for (Writer& w : g_writers)
        if (const uint64_t rip = w.rip.load())
            log("writewatch:   %8u x %s", w.count.load(), mem::module_offset(rip).c_str());
    if (g_exec) sample_summary();
}

void reset() {
    for (Writer& w : g_writers) {
        w.ready = false;
        w.logged = false;
        w.count = 0;
        w.rip = 0;
        std::memset(w.returns, 0, sizeof w.returns);
        std::memset(w.frames, 0, sizeof w.frames);
    }
    g_hits = 0;
    g_dropped = 0;
    g_sample_count = 0;
}

uintptr_t parse(const std::string& s) {
    try {
        return s.empty() ? 0 : uintptr_t(std::stoull(s, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

// The address of a WriteWatch value after its mode prefix: hex, or di[+<hex offset>] = the buffer of the game's last
// DirectInput GetDeviceState (dinputpad), e.g. di+8 = lZ of a DIJOYSTATE (the right stick's X on PlayStation pads).
// what: how it was resolved, for the log ("" for a plain address).
uintptr_t resolve(const std::string& s, std::string& what) {
    what.clear();
    for (Provider& p : g_providers) {
        const AddressFn fn = p.fn.load();
        const size_t n = std::strlen(p.name);
        if (!fn || s.compare(0, n, p.name) != 0 || (s.size() > n && s[n] != '+')) continue;
        const uintptr_t base = fn();
        const uintptr_t offset = s.size() > n + 1 ? parse(s.substr(n + 1)) : 0;
        if (!base) {
            what = std::string(p.name) + " has no object now";
            return 0;
        }
        char text[128];
        std::snprintf(text, sizeof text, "%s = 0x%llX +0x%llX", s.c_str(), (unsigned long long)base,
                      (unsigned long long)offset);
        what = text;
        return base + offset;
    }
    if (s.rfind("di", 0) != 0) return parse(s);
    uintptr_t buffer = 0;
    uint32_t size = 0;
    if (!dinputpad::last_state(buffer, size)) {
        what = "the game hasn't read a DirectInput controller with GetDeviceState yet";
        return 0;
    }
    const uintptr_t offset = s.size() > 3 && s[2] == '+' ? parse(s.substr(3)) : 0;
    char text[128];
    std::snprintf(text, sizeof text, "%s = the game's GetDeviceState buffer 0x%llX (%u bytes) +0x%llX", s.c_str(),
                  (unsigned long long)buffer, size, (unsigned long long)offset);
    what = text;
    return buffer + offset;
}

DWORD WINAPI watcher(void*) {
    std::string last;
    ULONGLONG armed_at = 0, last_apply = 0, seconds = 10;
    Mode armed_mode = Mode::write;
    std::string armed_name;  // the address as written, when it is an engine's name (followed while armed)
    for (;;) {
        Sleep(500);
        const std::string setting = settings::get_string(L"WriteWatch");
        const ULONGLONG now = GetTickCount64();
        if (setting != last) {
            last = setting;
            if (g_target.load()) {
                set_breakpoint(0);
                g_target = 0;
                log_new_writers();
                summary("disarmed (WriteWatch changed)");
            }
            if (setting.empty() || setting == "on") continue;
            const Mode mode = setting.rfind("x:", 0) == 0   ? Mode::execute
                              : setting.rfind("r:", 0) == 0 ? Mode::access
                                                            : Mode::write;
            std::string what;
            uintptr_t address = resolve(mode == Mode::write ? setting : setting.substr(2), what);
            if (mode != Mode::execute) address &= ~uintptr_t(3);
            uint32_t probe = 0;
            if (!mem::plausible(address, 4) || !mem::rd(address, probe)) {
                log("writewatch: WriteWatch=%s is not a readable address%s%s%s; not armed", setting.c_str(),
                    what.empty() ? "" : " (", what.c_str(), what.empty() ? "" : ")");
                continue;
            }
            static const bool handler = AddVectoredExceptionHandler(1, &on_exception) != nullptr;
            if (!handler) {
                log("writewatch: can't add the exception handler (error %lu)", GetLastError());
                continue;
            }
            reset();
            seconds = ULONGLONG(std::max(1, settings::get(L"WriteWatchSeconds", 10)));
            // Execute mode: WriteWatchFields=<hex offset>,... from rcx, sampled on every call.
            g_field_count = 0;
            const std::string fields = settings::get_string(L"WriteWatchFields");
            for (size_t at = 0; at < fields.size() && g_field_count < 6;) {
                const size_t comma = std::min(fields.find(',', at), fields.size());
                if (comma > at) g_fields[g_field_count++] = int32_t(parse(fields.substr(at, comma - at)));
                at = comma + 1;
            }
            armed_mode = mode;
            armed_name.clear();
            const std::string written = mode == Mode::write ? setting : setting.substr(2);
            for (Provider& p : g_providers)
                if (p.fn.load() && written.rfind(p.name, 0) == 0) armed_name = written;
            g_exec = mode == Mode::execute;
            g_previous = 0;
            g_target = address;
            const int threads = set_breakpoint(address, mode);
            armed_at = last_apply = now;
            float value = 0;
            std::memcpy(&value, &probe, 4);
            log("writewatch: armed on 0x%llX (%s%s%s, %s, now %.6f = int %d) in %d threads for %llu s",
                (unsigned long long)address, mem::module_offset(address).c_str(), what.empty() ? "" : ", ",
                what.c_str(), mode == Mode::execute ? "execute" : mode == Mode::access ? "read or write" : "write",
                value, int32_t(probe), threads, seconds);
            continue;
        }
        if (!g_target.load()) continue;
        log_new_writers();
        if (g_hits.load() >= kMaxHits) {
            set_breakpoint(0);
            g_target = 0;
            summary("too many hits, disarmed (this address is touched too often to watch)");
        } else if (now - armed_at >= seconds * 1000) {
            set_breakpoint(0);
            g_target = 0;
            summary("time up, disarmed (set another WriteWatch address to watch again)");
        } else {
            // A named address moved (the engine replaced the object): follow it.
            if (!armed_name.empty()) {
                std::string what;
                uintptr_t moved = resolve(armed_name, what);
                if (armed_mode != Mode::execute) moved &= ~uintptr_t(3);
                const uintptr_t old = g_target.load();
                if (moved && moved != old && mem::plausible(moved, 4)) {
                    g_previous = old;
                    g_target = moved;
                    set_breakpoint(moved, armed_mode);
                    last_apply = now;
                    log("writewatch: %s moved to 0x%llX (%s)", armed_name.c_str(), (unsigned long long)moved,
                        what.c_str());
                }
            }
            if (now - last_apply >= 1000) {
                set_breakpoint(g_target.load(), armed_mode);  // threads created since
                last_apply = now;
            }
        }
    }
}

}  // namespace

void provide(const char* name, AddressFn fn) {
    for (Provider& p : g_providers)
        if (!p.fn.load()) {
            strncpy_s(p.name, name, _TRUNCATE);
            p.fn = fn;
            return;
        }
}

void start() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_image_begin = base;
    g_image_end = base + nt->OptionalHeader.SizeOfImage;
    log("writewatch: ready; set WriteWatch=<hex address> in Gyro4All.ini to watch it (re-read every 500 ms)");
    if (HANDLE t = CreateThread(nullptr, 0, &watcher, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace gyro4all::writewatch
