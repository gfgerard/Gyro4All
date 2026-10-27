// RE Engine (Capcom): Resident Evil 7 and later. The main thread only waits in GetMessageW while the game loop runs on
// another thread, so this module supplies the gyro frame itself (Engine::frame_source): it hooks one of
// via.Application's module entries by swapping the entry's function pointer (data, no code patch), like REFramework.
// The frame runs just before that entry, on the thread that runs it.
//
// The camera depends on the game family (re_camera.hpp): the first model whose classes are in the TDB is used.
#include "re.hpp"
#include "re_camera.hpp"
#include "re_tdb.hpp"

#include "engine.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "settings.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <format>

namespace gyro4all::re {

namespace {

using EntryFn = void (*)(void* entry);

Engine::FrameFn g_frame = nullptr;
std::atomic<EntryFn> g_original{nullptr};
std::atomic<DWORD> g_frame_thread{0};
std::atomic<bool> g_other_thread_logged{false};
std::string g_entry_name;      // FrameEntry=
std::string g_tdb_dump;        // TdbDump=
bool g_camera_watch = false;   // CameraWatch=
std::atomic<int> g_stage{0};   // how far attach got, for the status line
Tdb g_tdb;                     // written by the attach thread before the hook goes in, read-only afterwards

const char* const kStages[] = {"waiting for the TDB",      "waiting for via.Application", "waiting for its entries",
                               "frame hooked",             "failed"};
enum Stage { WaitTdb, WaitApplication, WaitEntries, Hooked, Failed };

// The game's camera model: set up by the attach thread, published through g_camera once its singletons are found;
// used on the game thread only afterwards.
std::unique_ptr<Camera> g_camera_owner;
std::atomic<Camera*> g_camera{nullptr};
// No camera model fits the game, or its singletons never appeared: no hook for its camera (the gyro then goes to the
// game as right-stick input, EngineFrame::no_camera).
std::atomic<bool> g_no_camera{false};
CameraFrame g_frame_state;
std::string g_last_view;

const char kSettings[] =
    "; RE Engine: the via.Application step (module entry) before which the gyro runs each frame. Gyro4All.log lists\r\n"
    ";     the game's steps on start.\r\n"
    "FrameEntry=LateUpdateBehavior\r\n"
    "; RE Engine, for finding the camera of a new game: comma-separated parts of type names (e.g. camera,Player) whose\r\n"
    ";     fields Gyro4All writes to Gyro4All\\Gyro4All-tdb.txt on start, with the game's singletons. Empty = only when\r\n"
    ";     no camera model fits the game (then the types named *Camera*).\r\n"
    "TdbDump=\r\n"
    "; RE Engine, for finding what turns a camera: 1 = log the fields of the active camera object that change (every\r\n"
    ";     500 ms, with the objects it references). Makes the log large: leave it at 0 for playing.\r\n"
    "CameraWatch=0\r\n";

void entry_hook(void* entry) {
    const DWORD thread = GetCurrentThreadId();
    DWORD expected = 0;
    if (g_frame_thread.compare_exchange_strong(expected, thread)) {
        log("re: first gyro frame, before %s on thread %lu", g_entry_name.c_str(), thread);
        expected = thread;
    }
    // GyroLib has one owner thread: a step run elsewhere (should not happen) is passed through without a gyro frame.
    if (expected == thread) {
        g_frame();
    } else if (!g_other_thread_logged.exchange(true)) {
        log("re: %s also runs on thread %lu (gyro thread %lu); no gyro frame there", g_entry_name.c_str(), thread,
            expected);
    }
    g_original.load()(entry);
}

// Waits up to timeout_ms for step() to succeed, polling every 250 ms.
template <class F>
bool wait_for(ULONGLONG timeout_ms, F step) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (step()) return true;
        if (GetTickCount64() > deadline) return false;
        Sleep(250);
    }
}

void fail(const char* what) {
    g_stage = Failed;
    log("re: %s; the gyro can't run in this game", what);
}

void log_entries(const std::vector<AppEntry>& entries) {
    std::string line;
    for (const AppEntry& e : entries) {
        line += std::format("{}{}({})", line.empty() ? "" : " ", e.name, e.priority);
        if (line.size() > 100) {
            log("re:   %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) log("re:   %s", line.c_str());
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Attach thread: the fields of every type whose full name contains one of the comma-separated parts (any case; generic
// instances only for parts with a '<'), and the type of each singleton slot, to Gyro4All-tdb.txt. Works in memory, so also for exes that
// can't be read from disk (Xbox/Microsoft Store builds), where tools/reEngine/tdbdump.py can't help.
void dump_tdb(const std::string& filter, const std::vector<uintptr_t>& slots) {
    std::vector<std::string> parts;
    for (size_t at = 0; at <= filter.size();) {
        const size_t comma = std::min(filter.find(',', at), filter.size());
        std::string part = filter.substr(at, comma - at);
        part.erase(0, part.find_first_not_of(' '));
        part.erase(part.find_last_not_of(' ') + 1);
        if (!part.empty()) parts.push_back(lower(part));
        at = comma + 1;
    }
    const std::wstring path = settings::mod_dir() + L"\\Gyro4All-tdb.txt";
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") || !out) return log("re: can't write Gyro4All-tdb.txt");
    std::fprintf(out, "TDB %u, %u types, filter \"%s\"\n\nsingletons:\n", g_tdb.version, g_tdb.num_types, filter.c_str());
    for (uintptr_t slot : slots) {
        const uint32_t type = object_type(g_tdb, mem::rdp(slot));
        if (type) std::fprintf(out, "  %s %s\n", mem::module_offset(slot).c_str(), type_name(g_tdb, type).c_str());
    }
    size_t matched = 0;
    for (uint32_t i = 1; i < g_tdb.num_types; ++i) {
        const std::string name = type_name(g_tdb, i);
        if (name.empty()) continue;
        const std::string key = lower(name);
        // Generic instances only for parts that name one (e.g. "valuetrigger`1<").
        const bool generic = name.find('<') != std::string::npos;
        if (std::none_of(parts.begin(), parts.end(), [&](const std::string& p) {
                return key.find(p) != std::string::npos && (!generic || p.find('<') != std::string::npos);
            }))
            continue;
        ++matched;
        const uint32_t parent = parent_type(g_tdb, i);
        std::fprintf(out, "\n%s (type %u) : %s\n", name.c_str(), i, parent ? type_name(g_tdb, parent).c_str() : "-");
        for (const FieldDesc& f : own_fields(g_tdb, i)) {
            const std::string type = type_name(g_tdb, f.type);
            if (f.is_static) std::fprintf(out, "  static  %s %s\n", type.c_str(), f.name.c_str());
            else std::fprintf(out, "  +0x%-5x %s %s\n", f.offset, type.c_str(), f.name.c_str());
        }
    }
    std::fclose(out);
    log("re: TdbDump: %zu types matching \"%s\" written to Gyro4All-tdb.txt", matched, filter.c_str());
}

// Attach thread, after the hook: the first camera model whose classes are in the TDB, then its singletons.
void find_camera() {
    std::unique_ptr<Camera> camera;
    for (auto make : {&make_re7_camera, &make_re2_camera, &make_re4_camera, &make_requiem_camera, &make_pragmata_camera, &make_mhwilds_camera}) {
        std::unique_ptr<Camera> candidate = make();
        if (candidate->resolve(g_tdb)) {
            camera = std::move(candidate);
            break;
        }
    }
    std::vector<uintptr_t> slots = singleton_slots();
    log("re: %zu singleton slots in the exe", slots.size());
    // Protected code hides the slots' pattern: the managed objects held in the exe's data instead, rescanned until the
    // model's singletons are among them.
    const bool scan_data = slots.empty();
    if (!camera || !g_tdb_dump.empty()) {
        // The singletons are created during startup: give them time to appear in the dump.
        Sleep(15000);
        if (scan_data) slots = managed_object_slots(g_tdb);
        dump_tdb(g_tdb_dump.empty() ? "camera" : g_tdb_dump, slots);
    }
    if (!camera) {
        log("re: no known player camera in this game; the gyro goes to it as right-stick input");
        g_no_camera = true;
        return;
    }
    log("re: camera model: %s", camera->game());
    // The singletons are created during startup.
    if (!wait_for(300000, [&] {
            if (scan_data) {
                const ULONGLONG start = GetTickCount64();
                slots = managed_object_slots(g_tdb);
                log("re: %zu managed objects in the exe's data (%llu ms)", slots.size(), GetTickCount64() - start);
            }
            if (camera->find_singletons(slots)) return true;
            if (scan_data) Sleep(2000);
            return false;
        })) {
        log("re: the camera's singletons were not found in the exe's singleton slots; the gyro goes to the game as "
            "right-stick input");
        g_no_camera = true;
        return;
    }
    g_camera_owner = std::move(camera);
    g_camera.store(g_camera_owner.get(), std::memory_order_release);
}

DWORD WINAPI attach_thread(void*) {
    uintptr_t header = 0;
    uint32_t version = 0;
    // The TDB is filled in by the game's startup (after SteamStub and the engine's own init).
    if (!wait_for(180000, [&] {
            if (!header && (header = find_tdb_header(version)))
                log("re: TDB %u header at %s", version, mem::module_offset(header).c_str());
            if (header && !tdb_supported(version)) return true;
            return header && read_tdb(header, g_tdb);
        }))
        return fail(header ? "the TDB was never initialised" : "no TDB found in the exe"), 0;
    if (!tdb_supported(version)) return fail(std::format("TDB {} is not supported yet", version).c_str()), 0;
    log("re: TDB %u ready: %u types", g_tdb.version, g_tdb.num_types);

    g_stage = WaitApplication;
    const uint32_t app_type = find_type(g_tdb, "via", "Application");
    if (!app_type) return fail("via.Application not found in the TDB"), 0;
    uintptr_t application = 0;
    if (!wait_for(60000, [&] {
            const uintptr_t type = runtime_type(g_tdb, app_type);
            return type && (application = singleton_instance(type)) != 0;
        }))
        return fail("via.Application has no instance"), 0;
    log("re: via.Application (type %u) at %s", app_type, mem::module_offset(application).c_str());

    g_stage = WaitEntries;
    std::vector<AppEntry> entries;
    if (!wait_for(60000, [&] { return !(entries = application_entries(g_tdb, application)).empty(); }))
        return fail("via.Application's entries not found"), 0;
    log("re: %zu application entries at %s (name(priority), in run order):", entries.size(),
        mem::module_offset(entries.front().address).c_str());
    log_entries(entries);

    // The first step of that name (RE Requiem has two UpdateBehavior steps).
    const AppEntry* target = nullptr;
    for (const AppEntry& e : entries)
        if (!target && e.name == g_entry_name) target = &e;
    if (!target) return fail(std::format("FrameEntry={} is not one of them", g_entry_name).c_str()), 0;
    const uintptr_t original = mem::rdp(target->address + 8, 1);
    g_original = reinterpret_cast<EntryFn>(original);
    const EntryFn hook = &entry_hook;
    if (!original || !mem::guarded_store(target->address + 8, &hook, sizeof(hook)))
        return fail(std::format("can't hook {}", g_entry_name).c_str()), 0;
    g_stage = Hooked;
    log("re: gyro frame hooked before %s (was %s)", g_entry_name.c_str(), mem::module_offset(original).c_str());
    find_camera();
    return 0;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

class ReEngine final : public Engine {
public:
    const char* name() const override { return "RE"; }

    // The game's data: re_chunk_000.pak next to the exe.
    bool detect(const std::wstring& exe_path) const override {
        return exists(exe_path.substr(0, exe_path.find_last_of(L"\\/")) + L"\\re_chunk_000.pak");
    }

    const char* default_settings() const override { return kSettings; }

    void configure() override {
        g_entry_name = settings::get_string(L"FrameEntry", L"LateUpdateBehavior");
        g_tdb_dump = settings::get_string(L"TdbDump", L"");
        g_camera_watch = settings::get(L"CameraWatch", 0) != 0;
        log("settings: FrameEntry=%s%s%s", g_entry_name.c_str(), g_tdb_dump.empty() ? "" : " TdbDump=",
            g_tdb_dump.c_str());
    }

    std::vector<ViewInfo> views() const override { return {{1, "Default", 0}}; }

    bool frame_source(FrameFn frame) override {
        g_frame = frame;
        if (HANDLE thread = CreateThread(nullptr, 0, &attach_thread, nullptr, 0, nullptr)) {
            CloseHandle(thread);
            return true;
        }
        log("re: can't start the attach thread (error %lu)", GetLastError());
        return false;
    }

    // One view, the player's free look; the camera model leaves scripted cameras to the game.
    EngineFrame read_frame() override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        g_frame_state = camera ? camera->read() : CameraFrame{false, false, false, "no camera", "no camera"};
        const CameraFrame& c = g_frame_state;
        if (c.view != g_last_view) {
            g_last_view = c.view;
            log("re: camera %s", c.view.c_str());
        }
        if (g_camera_watch) {
            watch_fields(g_tdb, 0, c.object, GetTickCount64());
            watch_fields(g_tdb, 1, c.system, GetTickCount64());
        }
        EngineFrame f;
        f.in_game = c.in_game;
        f.paused = c.paused;
        f.view_id = c.free_look ? 1 : 0;
        f.target = c.target;
        if (!camera && g_no_camera) {
            f.no_camera = true;
            f.in_game = true;
            f.view_id = 1;
            f.target = "no camera hook (gyro as right stick)";
        }
        return f;
    }

    bool apply(double pitch, double yaw) override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        return camera && g_frame_state.free_look && camera->turn(pitch, yaw);
    }

    bool view_angles(double& pitch, double& yaw, uintptr_t& camera_id) override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        return camera && g_frame_state.free_look && camera->angles(pitch, yaw, camera_id);
    }

    Recenter recenter_support(std::string& detail) override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        double pitch = 0;
        if (!camera || !g_frame_state.free_look || !camera->pitch(pitch)) {
            detail = "(no free look)";
            return Recenter::NotYet;
        }
        detail = std::format("'Default' (level pitch 0, now {:.1f})", pitch);
        return Recenter::Yes;
    }

    bool recenter(double fraction) override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        double pitch = 0;
        if (!camera || !g_frame_state.free_look || !camera->pitch(pitch)) return false;
        return pitch == 0 || camera->turn(-pitch * fraction, 0);
    }

    std::string status_scene() const override {
        Camera* camera = g_camera.load(std::memory_order_acquire);
        return std::format("re: TDB {} {} {}", g_tdb.version, kStages[g_stage.load()],
                           camera ? camera->status() : "no camera model");
    }
};

}  // namespace

Engine& engine() {
    static ReEngine instance;
    return instance;
}

}  // namespace gyro4all::re
