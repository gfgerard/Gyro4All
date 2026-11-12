// Unit tests for the parts of Gyro4All that need no game: the Unreal module's game profile (ue_profile.cpp), the
// core's Match=Camera choice (camera_match.hpp), FOV conversion (fov.hpp), stick fallback (stick_fallback.cpp,
// stick_response.cpp), stick camera (stick_camera.cpp), sensor-time pacing (sensor_pacer.cpp), the Unity profile and
// aim-camera verdict (unity_profile.cpp, unity_aim.hpp) and zoom compensation (zoom.hpp). Built with Gyro4All (target
// gyro4all_tests); run with tools/test-gyro4all.sh.
#include "camera_match.hpp"
#include "fov.hpp"
#include "sensor_pacer.hpp"
#include "stick_camera.hpp"
#include "stick_fallback.hpp"
#include "ue_profile.hpp"
#include "ue_update_rotation_match.hpp"
#include "unity_aim.hpp"
#include "unity_candidates.hpp"
#include "unity_profile.hpp"
#include "unity_legacy_keys.hpp"
#include "unity_quat.hpp"
#include "util.hpp"
#include "zoom.hpp"

#include <array>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

using namespace gyro4all;
using namespace gyro4all::unreal;

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                          \
    } while (0)

std::wstring temp_profile(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"gyro4all_tests_" + name + L".ini";
    DeleteFileW(path.c_str());
    return path;
}

void write_file(const std::wstring& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

std::string read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string ini_value(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buffer[512];
    GetPrivateProfileStringW(section, key, L"", buffer, 512, path.c_str());
    char out[512];
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out, 512, nullptr, nullptr);
    return out;
}

const View* by_id(const std::vector<View>& views, uint32_t id) {
    for (const View& v : views)
        if (v.id == id) return &v;
    return nullptr;
}

// ---------------------------------------------------------------- CameraMatch

CameraMatch::Event step(CameraMatch& m, uint64_t now, double yaw, double moved, bool look = false,
                        bool write_failed = false, int rewritten = -1) {
    CameraMatch::Frame f;
    f.yaw = yaw, f.moved = moved, f.look = look, f.write_failed = write_failed, f.rewritten = rewritten;
    return m.frame(f, now);
}

// A simulated game, frame by frame (10 ms), driven the way ue_engine.cpp drives CameraMatch: the gyro writes `gyro` degrees
// of yaw into the current member's rotation; the camera follows only `driver` (0 = a cutscene). A member in
// `rewritten_by_game` that isn't the driver gets its old value put back by the game a frame later (an output, like
// PAYDAY 3's ControlRotation in a clamped action); any other non-driver keeps the write but the camera ignores it.
struct Sim {
    CameraMatch m;
    uint64_t now = 1000;
    uint32_t driver = 1;
    std::vector<uint32_t> rewritten_by_game;
    double value[16] = {};  // each member's rotation (yaw)
    uint32_t last_view = 0;
    double before = 0, after = 0, written = 0;
    double carried = 0;       // gyro handed over on switches
    double camera_moved = 0;  // total camera rotation
    CameraMatch::Event last = CameraMatch::None;

    explicit Sim(std::vector<uint32_t> members) { m.select(members); }

    bool rewrites(uint32_t id) const {
        return id != driver && std::find(rewritten_by_game.begin(), rewritten_by_game.end(), id) != rewritten_by_game.end();
    }
    // Runs `ms` milliseconds; returns the last non-None event.
    CameraMatch::Event run(uint64_t ms, double gyro = 0.3, double pitch = 0) {
        last = CameraMatch::None;
        for (uint64_t end = now + ms; now < end; now += 10) {
            const uint32_t id = m.current();
            // The game's frame: an output member gets its old value back.
            if (last_view && rewrites(last_view)) value[last_view] = before;
            int verdict = -1;
            if (last_view == id) verdict = CameraMatch::rewritten(before, after, value[id], written);
            before = value[id];
            value[id] += gyro;
            after = value[id];
            written = gyro;
            last_view = id;
            const double moved = id == driver ? gyro : 0;
            camera_moved += moved;
            CameraMatch::Frame f;
            f.yaw = gyro, f.pitch = pitch, f.moved = moved, f.rewritten = verdict;
            const CameraMatch::Event e = m.frame(f, now);
            if (e == CameraMatch::Switched) {
                carried += std::abs(m.carry_yaw());
                if (m.current() == driver) camera_moved += std::abs(m.carry_yaw());
                last_view = 0;
            }
            if (e != CameraMatch::None) last = e;
        }
        return last;
    }
};

void test_rewritten_helper() {
    std::puts("match: rewritten() tells a put-back write from a kept one");
    CHECK(CameraMatch::rewritten(10, 10.5, 10.5, 0.5) == 0);          // kept
    CHECK(CameraMatch::rewritten(10, 10.5, 10.0, 0.5) == 1);          // put back
    CHECK(CameraMatch::rewritten(10, 10.5, 10.02, 0.5) == 1);         // put back, almost exactly
    CHECK(CameraMatch::rewritten(10, 10.01, 10.0, 0.01) == -1);       // too small to tell
    CHECK(CameraMatch::rewritten(10, 10.5, 13.0, 0.5) == -1);         // moved for other reasons
    CHECK(CameraMatch::rewritten(179.8, -179.7, 179.8, 0.5) == 1);    // across +-180
    CHECK(CameraMatch::rewritten(179.8, -179.7, -179.7, 0.5) == 0);
    CHECK(CameraMatch::rewritten(10, 9.5, 10.0, -0.5) == 1);          // negative writes
}

void test_match_single_member() {
    std::puts("match: one member never switches");
    Sim s({1});
    s.driver = 0;
    CHECK(s.run(5000) == CameraMatch::None);
    CHECK(s.m.current() == 1);
    CHECK(!s.m.active());
}

void test_match_base_follows() {
    std::puts("match: the base view stays while the camera follows it");
    Sim s({1, 5});
    CHECK(s.m.active());
    CHECK(s.run(4000) == CameraMatch::None);
    CHECK(s.m.current() == 1);
    CHECK(s.carried == 0);
}

void test_match_fast_path() {
    std::puts("match: a base view the game rewrites is dropped within a few frames, its gyro carried over");
    Sim s({1, 5});
    s.rewritten_by_game = {1, 5};
    s.run(1000);
    s.driver = 5;  // the clamped action starts: ControlRotation becomes an output
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 1000 && e != CameraMatch::Switched) e = s.run(10);
    CHECK(e == CameraMatch::Switched);
    CHECK(s.m.reason() == CameraMatch::Rewritten);
    CHECK(s.now - start <= 60);
    CHECK(s.m.current() == 5);
    CHECK(s.carried > 0.8 && s.carried < 1.6);  // the 3-4 writes that were put back
    CHECK(s.run(500) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
}

void test_match_slow_path() {
    std::puts("match: a rotation the camera ignores without rewriting is dropped by the camera check, carried over");
    Sim s({1, 5});
    s.run(1000);
    s.driver = 5;  // ControlRotation keeps the writes, but the camera now ignores it
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 1000 && e != CameraMatch::Switched) e = s.run(10);
    CHECK(e == CameraMatch::Switched);
    CHECK(s.m.reason() == CameraMatch::Ignored);
    CHECK(s.now - start >= CameraMatch::kJudgeMinMs && s.now - start <= 400);
    CHECK(s.carried >= CameraMatch::kJudgeDegrees);
    CHECK(s.m.current() == 5);
}

void test_match_scene_round_trip() {
    std::puts("match: normal play -> clamped action -> normal play, no gyro lost");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.run(2000);
    CHECK(s.m.current() == 1);
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
    s.driver = 1;  // the action ends; the modifier's rotation is just ignored now
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 1);
    CHECK(s.run(2000) == CameraMatch::None);
    // Everything written reached the camera, directly or carried over, and no more (to a frame or two).
    const double total = 0.3 * 600;
    CHECK(std::abs(s.camera_moved - total) < 0.7);
    CHECK(s.carried > 1);
}

void test_match_single_rewrite_doesnt_switch() {
    std::puts("match: one write put back (recoil, a clamp) doesn't drop the view");
    CameraMatch m;
    m.select({1, 5});
    uint64_t now = 1000;
    CHECK(step(m, now, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 0) == CameraMatch::None);  // kept: streak over
    CHECK(step(m, now += 10, 0.3, 0.3, false, false, 1) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_lagging_camera_follows() {
    std::puts("match: a camera that follows with lag isn't taken as ignoring the gyro");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    CHECK(step(m, now, 3, 0) == CameraMatch::None);
    for (int i = 1; i <= 15; ++i) CHECK(step(m, now + i * 10, 0, 0.2) == CameraMatch::None);
    CHECK(step(m, now + 210, 0, 0) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_needs_enough_gyro_and_time() {
    std::puts("match: no camera-check verdict below 2 degrees or before 200 ms");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    for (int i = 0; i < 15; ++i) CHECK(step(m, now + i * 10, 0.1, 0) == CameraMatch::None);  // 1.5 degrees
    CHECK(m.current() == 1);
    CameraMatch f;
    f.select({1, 5});
    CHECK(step(f, now, 5, 0) == CameraMatch::None);
    CHECK(step(f, now + 100, 0, 0) == CameraMatch::None);
    CHECK(step(f, now + 210, 0, 0) == CameraMatch::Switched);
    CHECK(f.carry_yaw() == 5);
}

void test_match_look_input_resets() {
    std::puts("match: stick or mouse input prevents a verdict");
    CameraMatch m;
    m.select({1, 5});
    const uint64_t now = 1000;
    for (int i = 0; i < 200; ++i) CHECK(step(m, now + i * 10, 0.3, 0, true, false, 1) == CameraMatch::None);
    CHECK(m.current() == 1);
}

void test_match_cutscene() {
    std::puts("match: a cutscene ignores every view -> base view, rest, nothing carried; then found again");
    Sim s({1, 5, 6});
    s.driver = 0;
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 3000 && e != CameraMatch::AllIgnored) e = s.run(10);
    CHECK(e == CameraMatch::AllIgnored);
    CHECK(s.m.current() == 1);
    CHECK(s.m.carry_yaw() == 0);
    const double carried = s.carried;
    CHECK(s.run(CameraMatch::kRestMs - 400) == CameraMatch::None);  // resting
    CHECK(s.carried == carried);
    s.driver = 5;
    CHECK(s.run(2000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
}

void test_match_write_failed() {
    std::puts("match: a view whose object is missing is dropped at once, with that frame's gyro carried over");
    CameraMatch m;
    m.select({1, 5});
    uint64_t now = 1000;
    for (int i = 0; i < 30; ++i) step(m, now += 10, 0.3, 0);
    CHECK(m.current() == 5);
    for (int i = 0; i < 30; ++i) step(m, now += 10, 0.3, 0.3);
    CHECK(step(m, now += 10, 0.4, 0, false, true) == CameraMatch::Switched);
    CHECK(m.reason() == CameraMatch::Missing);
    CHECK(m.current() == 1);
    CHECK(m.carry_yaw() == 0.4);
    CameraMatch n;
    n.select({1, 5});
    CHECK(step(n, now, 0.3, 0, false, true) == CameraMatch::Switched);
    CHECK(step(n, now, 0.3, 0, false, true) == CameraMatch::AllIgnored);
    CHECK(n.current() == 1);
    CHECK(n.carry_yaw() == 0);
}

void test_match_carry_limit() {
    std::puts("match: at most 45 degrees per axis are carried over");
    CameraMatch m;
    m.select({1, 5});
    CHECK(step(m, 1000, 80, 0) == CameraMatch::None);
    CHECK(step(m, 1300, 0, 0) == CameraMatch::Switched);
    CHECK(m.carry_yaw() == CameraMatch::kMaxCarryYaw);
}

void test_match_pitch_limit() {
    std::puts("match: pushing pitch against the limit (camera still) is no camera change");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    CHECK(s.m.current() == 5);
    // Looking straight up and still pushing: only pitch is written, the camera doesn't move.
    CHECK(s.run(3000, 0, 0.5) == CameraMatch::None);
    CHECK(s.m.current() == 5);
    // Pushing up while turning sideways: the yaw still follows.
    CHECK(s.run(3000, 0.3, 1.0) == CameraMatch::None);
    CHECK(s.m.current() == 5);
}

void test_match_all_ignored_keeps_followed() {
    std::puts("match: when every view is ignored, the one followed last stays (a yaw limit in the clamped action)");
    Sim s({1, 5});
    s.rewritten_by_game = {1};
    s.driver = 5;
    CHECK(s.run(1000) == CameraMatch::Follows);
    s.driver = 0;  // held at its yaw limit: the clamp view is ignored, the base view is still rewritten
    const uint64_t start = s.now;
    CameraMatch::Event e = CameraMatch::None;
    while (s.now < start + 3000 && e != CameraMatch::AllIgnored) e = s.run(10);
    CHECK(e == CameraMatch::AllIgnored);
    CHECK(s.m.current() == 5);
    s.driver = 5;  // back inside the limits: no switch needed
    CHECK(s.run(3000) == CameraMatch::Follows);  // confirmed again, no switch
    CHECK(s.m.current() == 5);
}

void test_match_pitch_carry_limit() {
    std::puts("match: at most 10 degrees of pitch are carried over");
    CameraMatch m;
    m.select({1, 5});
    CameraMatch::Frame f;
    f.yaw = 3, f.pitch = 30;
    CHECK(m.frame(f, 1000) == CameraMatch::None);
    CHECK(step(m, 1300, 0, 0) == CameraMatch::Switched);
    CHECK(m.carry_yaw() == 3);
    CHECK(m.carry_pitch() == CameraMatch::kMaxCarryPitch);
}

void test_match_members_change() {
    std::puts("match: new members (view target or profile changed) start over at the base view");
    Sim s({1, 5});
    s.driver = 5;
    s.run(500);
    CHECK(s.m.current() == 5);
    CHECK(s.m.select({1, 5, 7}) == 1);
    CHECK(s.m.select({2}) == 2);
}

// ---------------------------------------------------------------- FOV

void test_fov_conversion() {
    std::puts("fov: horizontal FOV converts to vertical with the camera aspect ratio");
    CHECK(std::abs(fov::vertical_from_horizontal(90, 1) - 90) < 1e-9);
    CHECK(std::abs(fov::vertical_from_horizontal(90, 16.0 / 9) - 58.7155) < 0.0001);
    CHECK(fov::vertical_from_horizontal(0, 16.0 / 9) == 0);
    CHECK(fov::vertical_from_horizontal(179, 16.0 / 9) == 0);
    CHECK(fov::vertical_from_horizontal(90, 0) == 0);
    CHECK(fov::vertical_from_horizontal(90, -1) == 0);
    CHECK(!fov::is_valid(179));
}

// ---------------------------------------------------------------- profile

void test_load_missing() {
    std::puts("profile: a missing file gives the default view alone");
    const std::wstring path = temp_profile(L"missing");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 1);
    CHECK(views[0].id == 1);
    CHECK(views[0].label == "Exploration");
    CHECK(!has_views(path));
}

void test_load_views() {
    std::puts("profile: ViewTargetClass and Match=Camera views load, invalid ones are skipped");
    const std::wstring path = temp_profile(L"load");
    write_file(path,
               "[Gyro4All]\r\nViews=InCar, Clamp, Bad, Dup\r\n\r\n"
               "[View.Default]\r\nLabel=On foot\r\nObject=Camera.@BendPlayerCamFollow_C\r\nOffsets=0x120\r\nMatrix=1\r\n"
               "ZoomReferenceFOV=90\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\nZoomReferenceFOV=50\r\n\r\n"
               "[View.Clamp]\r\nId=3\r\nMatch=camera\r\nObject=Camera.@SBZClampCameraRotationModifier\r\nOffsets=0x68\r\n"
               "RotatorSize=24\r\n\r\n"
               "[View.Bad]\r\nId=4\r\nMatch=Camera\r\n\r\n"
               "[View.Dup]\r\nId=3\r\nMatch=Camera\r\nProperty=X\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(views[0].label == "On foot");
    CHECK(views[0].matrix && views[0].offsets.size() == 1 && views[0].offsets[0] == 0x120);
    CHECK(views[0].zoom_reference_fov == 90);
    const View* car = by_id(views, 2);
    CHECK(car && car->view_target_class == "BP_Car_C" && !car->match_camera && car->property == "CamRot");
    CHECK(car && car->zoom_reference_fov == 50);
    const View* clamp = by_id(views, 3);
    CHECK(clamp && clamp->match_camera && clamp->view_target_class.empty());
    CHECK(clamp && clamp->object.size() == 2 && clamp->object[1] == "@SBZClampCameraRotationModifier");
    CHECK(clamp && clamp->offsets.size() == 1 && clamp->offsets[0] == 0x68 && clamp->rotator_size == 24);
    CHECK(clamp && clamp->label == "Clamp" && clamp->parent == 1);
    CHECK(has_views(path));
}

void test_load_active_list_views() {
    std::puts("profile: ActiveList views (one camera object per mode) load without ViewTargetClass or Match");
    const std::wstring path = temp_profile(L"active_list");
    write_file(path,
               "[Gyro4All]\r\nViews=Vehicle, Aim\r\n\r\n"
               "[View.Default]\r\nObject=Camera.@BendPlayerCamFollow_C|BendCamMelee\r\nOffsets=0x120\r\nMatrix=1\r\n"
               "OutputMatrix=0x80\r\nActiveList=0x35D0\r\nActiveListSkip=BendCamTransition\r\n\r\n"
               "[View.Vehicle]\r\nId=2\r\nObject=Camera.@BendVehicleCamFollow_C\r\nOffsets=0x120\r\nMatrix=1\r\n"
               "ActiveList=0x35D0\r\n\r\n"
               "[View.Aim]\r\nId=3\r\nObject=Camera.@BendCamManualAim\r\nOffsets=0x120\r\nMatrix=1\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 2);  // Aim has no ActiveList, ViewTargetClass or Match: skipped
    CHECK(views[0].active_list == 0x35D0 && views[0].output_matrix == 0x80 && views[0].active_skip.size() == 1 &&
          views[0].active_skip[0] == "BendCamTransition");
    CHECK(views[0].object.size() == 2 && views[0].object[1] == "@BendPlayerCamFollow_C|BendCamMelee");
    const View* vehicle = by_id(views, 2);
    CHECK(vehicle && vehicle->active_list == 0x35D0 && vehicle->output_matrix < 0 && vehicle->parent == 1);
}

LearnedView clamp_view() {
    LearnedView l;
    l.object = "Camera.@SBZClampCameraRotationModifier";
    l.offsets = {0x68};
    l.rotator_size = 24;
    l.label = "SBZClampCameraRotationModifier";
    l.note = "Learned by a test.\nSecond line.";
    return l;
}

// Hand-assembled function starts (x64, this in rcx), padded with int3 so the decoder never reads past the end.
std::vector<uint8_t> code(std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    v.resize(v.size() + 32, 0xCC);
    return v;
}

void test_update_rotation_shape() {
    std::puts("update rotation: a function that reads RotationInput first is recognized by its shape");
    // mov r11, rsp; push rbp; sub rsp, 0x150; mov eax, [rcx+0x460]; movsd xmm0, [rcx+0x458]: Dead Island 2's
    // UpdateRotation starts like this (roll, then pitch and yaw).
    const auto whole = code({0x4C, 0x8B, 0xDC, 0x55, 0x48, 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00, 0x8B, 0x81, 0x60, 0x04, 0x00, 0x00,
                             0xF2, 0x0F, 0x10, 0x81, 0x58, 0x04, 0x00, 0x00});
    CHECK(reads_this_field_first(whole.data(), whole.size(), 0x458, 12));
    CHECK(!reads_this_field_first(whole.data(), whole.size(), 0x2C0, 12));  // another field
    // movups xmm0, [rcx+0x458]; movsd xmm1, [rcx+0x468] (UE5's double rotator: 24 bytes).
    const auto wide = code({0x0F, 0x10, 0x81, 0x58, 0x04, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x89, 0x68, 0x04, 0x00, 0x00});
    CHECK(reads_this_field_first(wide.data(), wide.size(), 0x458, 24));
    // One component, then a call: AddYawInput (Dead Island 2: movss xmm6, [rcx+0x45C]; call ...) isn't UpdateRotation.
    const auto one = code({0x53, 0x48, 0x8B, 0xD9, 0xF3, 0x0F, 0x10, 0xB1, 0x5C, 0x04, 0x00, 0x00, 0xE8, 0, 0, 0, 0,
                           0xF3, 0x0F, 0x10, 0x83, 0x5C, 0x04, 0x00, 0x00});
    CHECK(!reads_this_field_first(one.data(), one.size(), 0x458, 12));
    // The same component twice isn't the whole rotator either.
    const auto twice = code({0x8B, 0x81, 0x5C, 0x04, 0x00, 0x00, 0x8B, 0x91, 0x5C, 0x04, 0x00, 0x00});
    CHECK(!reads_this_field_first(twice.data(), twice.size(), 0x458, 12));
    // A store isn't a read: mov [rcx+0x458], rax (the tick zeroing RotationInput).
    const auto store = code({0x48, 0x89, 0x81, 0x58, 0x04, 0x00, 0x00});
    CHECK(!reads_this_field_first(store.data(), store.size(), 0x458, 12));
    // After a call: not the first statement.
    const auto after_call = code({0xE8, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x81, 0x60, 0x04, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x81, 0x58,
                                  0x04, 0x00, 0x00});
    CHECK(!reads_this_field_first(after_call.data(), after_call.size(), 0x458, 12));
    // rcx replaced first (mov rcx, [rcx+8]): the read is on another object.
    const auto other_object = code({0x48, 0x8B, 0x49, 0x08, 0x8B, 0x81, 0x60, 0x04, 0x00, 0x00, 0xF2, 0x0F, 0x10, 0x81, 0x58, 0x04,
                                    0x00, 0x00});
    CHECK(!reads_this_field_first(other_object.data(), other_object.size(), 0x458, 12));
    // Another base register (rdx): not this.
    const auto rdx = code({0x8B, 0x82, 0x60, 0x04, 0x00, 0x00});
    CHECK(!reads_this_field_first(rdx.data(), rdx.size(), 0x458, 12));

}

void test_add_view_new_file() {
    std::puts("profile: add_view creates the profile and the view reads back");
    const std::wstring path = temp_profile(L"new");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "PAYDAY3-Win64-Shipping", clamp_view(), {1001, 1002, 1003}, id, key) == AddResult::Added);
    CHECK(id == 2);
    CHECK(key == "Auto2");
    CHECK(ini_value(path, L"Gyro4All", L"Views") == "Auto2");
    const std::string text = read_file(path);
    CHECK(text.find("; Gyro4All profile for PAYDAY3-Win64-Shipping") == 0);
    CHECK(text.find("; Learned by a test.\r\n; Second line.\r\n[View.Auto2]") != std::string::npos);
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 2);
    const View* v = by_id(views, 2);
    CHECK(v && v->match_camera && v->parent == 1 && v->label == "SBZClampCameraRotationModifier");
    CHECK(v && v->offsets.size() == 1 && v->offsets[0] == 0x68 && v->rotator_size == 24);
    CHECK(v && v->object.size() == 2 && v->object[0] == "Camera");
    // The default view stays ControlRotation.
    CHECK(views[0].object.empty() && views[0].property.empty() && views[0].offsets.empty());
    CHECK(v && v->pitch_view == 0);
}

void test_add_view_yaw_only() {
    std::puts("profile: a yaw-only learned view reads back with PitchView=1 (Dead Island 2's capsule)");
    const std::wstring path = temp_profile(L"yawonly");
    LearnedView capsule;
    capsule.object = "Pawn.CollisionCylinder";
    capsule.offsets = {0x200, 0x220};
    capsule.rotator_size = 12;
    capsule.pitch_view = 1;
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "DeadIsland-Win64-Shipping", capsule, {}, id, key) == AddResult::Added);
    const View* v = nullptr;
    const std::vector<View> views = load_views(path);
    v = by_id(views, id);
    CHECK(v && v->pitch_view == 1 && v->offsets.size() == 2 && v->rotator_size == 12);
    CHECK(views[0].pitch_view == 0);
}

void test_add_view_keeps_profile() {
    std::puts("profile: add_view keeps the existing views and settings, takes a free id");
    const std::wstring path = temp_profile(L"existing");
    write_file(path,
               "; hand-written profile\r\n[Gyro4All]\r\nViews=InCar\r\nDualSenseAsDualShock4=1\r\n\r\n"
               "[View.Default]\r\nLabel=On foot\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\n");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {1001, 1002, 1003}, id, key) == AddResult::Added);
    CHECK(id == 3);
    CHECK(ini_value(path, L"Gyro4All", L"Views") == "InCar,Auto3");
    CHECK(ini_value(path, L"Gyro4All", L"DualSenseAsDualShock4") == "1");
    CHECK(read_file(path).find("; hand-written profile") == 0);
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(views[0].label == "On foot");
    CHECK(by_id(views, 2) && by_id(views, 2)->property == "CamRot");
    CHECK(by_id(views, 3) && by_id(views, 3)->match_camera);
}

void test_add_view_legacy_profile() {
    std::puts("profile: a GyroUE profile ([GyroUE] Views=) keeps its views when add_view writes [Gyro4All] Views=");
    const std::wstring path = temp_profile(L"legacy");
    write_file(path,
               "[GyroUE]\r\nViews=InCar\r\nDualSenseAsDualShock4=1\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\n");
    CHECK(has_views(path));
    CHECK(load_views(path).size() == 2);
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {1001, 1002, 1003}, id, key) == AddResult::Added);
    CHECK(id == 3);
    CHECK(ini_value(path, L"Gyro4All", L"Views") == "InCar,Auto3");
    CHECK(ini_value(path, L"GyroUE", L"Views") == "InCar");  // left as it was; [Gyro4All] wins
    CHECK(ini_value(path, L"GyroUE", L"DualSenseAsDualShock4") == "1");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(by_id(views, 2) && by_id(views, 2)->property == "CamRot");
    CHECK(by_id(views, 3) && by_id(views, 3)->match_camera);
}

void test_add_view_duplicate() {
    std::puts("profile: the same rotation isn't added twice");
    const std::wstring path = temp_profile(L"dup");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {}, id, key) == AddResult::Added);
    const std::string before = read_file(path);
    CHECK(add_view(path, "Game", clamp_view(), {}, id, key) == AddResult::Duplicate);
    CHECK(id == 2 && key == "Auto2");
    CHECK(read_file(path) == before);
    // ControlRotation is the default view already.
    LearnedView control;
    control.object = "PC";
    control.property = "ControlRotation";
    CHECK(add_view(path, "Game", control, {}, id, key) == AddResult::Duplicate);
    CHECK(id == 1);
    // A different raw offset of the same object is another rotation.
    LearnedView other = clamp_view();
    other.offsets = {0x80};
    CHECK(add_view(path, "Game", other, {}, id, key) == AddResult::Added);
    CHECK(id == 3);
}

void test_add_view_reserved_ids() {
    std::puts("profile: reserved ids (sensitivity profiles) and ids in use are skipped");
    const std::wstring path = temp_profile(L"reserved");
    write_file(path, "[Gyro4All]\r\nViews=A\r\n[View.A]\r\nId=3\r\nViewTargetClass=X\r\nProperty=Y\r\n");
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", clamp_view(), {2, 4}, id, key) == AddResult::Added);
    CHECK(id == 5);
}

void test_load_trailing_tab() {
    std::puts("profile: a hand-edited Views= with a trailing space and tab still finds its view");
    const std::wstring path = temp_profile(L"tab");
    write_file(path, "[View.Default]\r\nLabel=Exploration\r\n[View.Clamp]\r\nId=2\r\nLabel=Clamp Camera\r\nMatch=Camera\r\n"
                     "Object=Camera.@SBZClampCameraRotationModifier\r\nOffsets=0x68\r\n[Gyro4All]\r\nViews=Clamp \t\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 2);
    CHECK(by_id(views, 2) && by_id(views, 2)->label == "Clamp Camera");
}

void test_add_view_property() {
    std::puts("profile: a reflected rotation is written as Property (with WorldRotation)");
    const std::wstring path = temp_profile(L"property");
    LearnedView l;
    l.object = "Pawn.CameraComponent";
    l.property = "RelativeRotation";
    l.world_rotation = true;
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", l, {}, id, key) == AddResult::Added);
    const std::vector<View> views = load_views(path);
    const View* v = by_id(views, id);
    CHECK(v && v->property == "RelativeRotation" && v->world_rotation && v->offsets.empty());
    CHECK(v && v->label == key);  // no label given: the section key
}

void test_add_view_rejects_empty() {
    std::puts("profile: a learned view without a rotation is refused");
    const std::wstring path = temp_profile(L"empty");
    LearnedView l;
    l.object = "Camera";
    uint32_t id = 0;
    std::string key;
    CHECK(add_view(path, "Game", l, {}, id, key) == AddResult::Failed);
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
}

void test_split() {
    std::puts("profile: split trims and drops empty items");
    const std::vector<std::string> parts = split(" a, b ,,c ", ',');
    CHECK(parts.size() == 3 && parts[0] == "a" && parts[1] == "b" && parts[2] == "c");
    CHECK(split("", ',').empty());
    const std::vector<std::string> tabs = split("Clamp \t", ',');
    CHECK(tabs.size() == 1 && tabs[0] == "Clamp");
}

}  // namespace

// ---- stick fallback: a simulated game whose camera turns by the gyro's writes (unless it undoes them) and by its own
// right stick, 60 frames/s. Its stick: an axial or radial dead zone, a curve, a top speed per axis, read `lag` frames
// late and smoothed (seconds, first order), pitch inverted or clamped.
constexpr double kPi = 3.14159265358979;

struct StickSim {
    StickFallback fb;
    double pitch = 0, yaw = 0;
    bool undoes = false;       // the game rebuilds the angles: writes don't last
    bool reads_stick = true;   // the game reads the controller through a path Gyro4All changes
    bool radial = false;
    double deadzone = 0.12, gamma = 1, speed = 81, speed_pitch = 81;
    double smooth = 0;
    double pitch_limit = 1e9;
    int lag = 0;
    bool inverted = false;
    // Frame time: 1/fps, varied by up to `jitter` (share) each frame. Past `edge` the stick's speed grows by edge_gain per
    // second held there (id Tech's acceleration). Motion that isn't ours: `noise` degrees per frame at most on both
    // axes, and a recoil kick of `recoil` degrees up every second.
    double fps = 60, jitter = 0, dt = 1.0 / 60;
    double edge = 2, edge_gain = 0, held = 0;
    double noise = 0, recoil = 0;
    int frames = 0;
    uint32_t seed = 12345;
    uintptr_t camera = 1;
    uint64_t t = 1;
    float x = 0, y = 0;  // the stick given this frame
    double fx = 0, fy = 0;
    float hx[16] = {}, hy[16] = {};

    double random() {  // -1..1
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / double(1u << 23) - 1;
    }
    // A frame of gyro speeds (degrees/s) over this frame's time.
    void move(double pitch_speed, double yaw_speed) { frame(pitch_speed * dt, yaw_speed * dt); }

    void frame(double gyro_pitch, double gyro_yaw, bool player_stick = false, double player_yaw = 0) {
        fb.frame(true, pitch, yaw, camera, player_stick, t);
        bool written = false;
        if (fb.direct() && (gyro_pitch || gyro_yaw)) {
            written = true;
            if (!undoes) pitch += gyro_pitch, yaw += gyro_yaw;
        }
        if (gyro_pitch || gyro_yaw) fb.turn(gyro_pitch, gyro_yaw, written);
        if (!fb.stick(x, y)) x = y = 0;
        for (int i = 15; i > 0; --i) hx[i] = hx[i - 1], hy[i] = hy[i - 1];
        hx[0] = x, hy[0] = y;
        const double k = dt / (smooth + dt);
        fx += (hx[lag] - fx) * k, fy += (hy[lag] - fy) * k;
        double sx = 0, sy = 0;
        const auto share = [&](double m) { return m <= deadzone ? 0.0 : std::pow(std::min(1.0, (m - deadzone) / (1 - deadzone)), gamma); };
        if (radial) {
            const double m = std::hypot(fx, fy);
            if (m > 0) sx = fx / m * share(m), sy = fy / m * share(m);
        } else {
            sx = std::copysign(share(std::abs(fx)), fx);
            sy = std::copysign(share(std::abs(fy)), fy);
        }
        held = std::max(std::abs(fx), std::abs(fy)) >= edge ? held + dt : 0;
        const double boost = 1 + edge_gain * held;
        if (reads_stick) {
            yaw += sx * speed * boost * dt;
            pitch += sy * speed_pitch * boost * dt * (inverted ? -1 : 1);
        }
        yaw += player_yaw;
        if (noise) yaw += noise * random(), pitch += noise * random();
        if (recoil && ++frames % int(fps) == 0) pitch += recoil;
        pitch = std::clamp(pitch, -pitch_limit, pitch_limit);
        t += uint64_t(dt * 1e9);
        dt = 1 / fps * (1 + jitter * random());
    }
};

StickSim forced_sim() {
    StickSim s;
    s.undoes = true;  // the game rebuilds the angles: only the stick turns the camera
    s.fb.force(true);
    return s;
}

// A player aiming: turns of every speed on both axes, slow and fast, `seconds` long.
void aim(StickSim& s, double seconds) {
    for (double t = 0; t < seconds; t += s.dt) {
        const double envelope = 0.2 + 0.8 * std::abs(std::sin(2 * kPi * t / 9));
        const double yaw = (100 * std::sin(2 * kPi * t / 2.1) + 15 * std::sin(2 * kPi * t / 0.7)) * envelope;
        const double pitch = 35 * std::sin(2 * kPi * t / 2.9) * (0.3 + 0.7 * std::abs(std::sin(2 * kPi * t / 6.5)));
        s.move(pitch, yaw);
    }
}

// Steady gyro speeds (degrees/s) for `seconds`; returns the camera's turn over them (yaw, pitch).
std::pair<double, double> hold(StickSim& s, double pitch, double yaw, double seconds) {
    const double from_yaw = s.yaw, from_pitch = s.pitch;
    for (double t = 0; t < seconds; t += s.dt) s.move(pitch, yaw);
    return {s.yaw - from_yaw, s.pitch - from_pitch};
}

// The camera's turn over a second of steady gyro speed (after half a second to settle), over the gyro's (measured
// against the gyro's own turn, at any frame rate); then back.
double yaw_ratio(StickSim& s, double yaw) {
    hold(s, 0, yaw, 0.5);
    double gyro = 0;
    const double from = s.yaw;
    for (double t = 0; t < 1; t += s.dt) gyro += yaw * s.dt, s.move(0, yaw);
    const double r = (s.yaw - from) / gyro;
    hold(s, 0, -yaw, 1.5);
    return r;
}

double pitch_ratio(StickSim& s, double pitch) {
    hold(s, pitch, 0, 0.5);
    double gyro = 0;
    const double from = s.pitch;
    for (double t = 0; t < 1; t += s.dt) gyro += pitch * s.dt, s.move(pitch, 0);
    const double r = (s.pitch - from) / gyro;
    hold(s, -pitch, 0, 1.5);
    return r;
}

// Learns for `seconds`, then: the camera turns at the gyro's speed within `tolerance` (shares) on both axes, and
// stands still with the gyro (no drift).
void check_learns(StickSim& s, double seconds, double tolerance, const char* name) {
    aim(s, seconds);
    CHECK(s.fb.learnt());
    s.noise = s.recoil = 0;  // learnt with them; measured without (they aren't the stick's)
    std::string ratios;
    for (double speed : {8.0, 20.0, 45.0}) {
        const double r = yaw_ratio(s, speed);
        ratios += std::format(" yaw {:.0f}: {:.2f}", speed, r);
        CHECK(std::abs(r - 1) < tolerance);
    }
    for (double speed : {8.0, 25.0}) {
        const double r = pitch_ratio(s, speed);
        ratios += std::format(" pitch {:.0f}: {:.2f}", speed, r);
        CHECK(std::abs(r - 1) < tolerance);
    }
    std::printf("stick fallback, %s:\n  learnt %s\n  camera/gyro at deg/s:%s\n", name, s.fb.model().describe().c_str(),
                ratios.c_str());
    // No drift: the gyro stops, the stick centres and the camera stops (what is left of the last turn goes out within a
    // moment; a game with motion of its own still moves by that).
    hold(s, 0, 0, 1);
    const double yaw = s.yaw, pitch = s.pitch;
    bool centred = true;
    for (double t = 0; t < 10; t += s.dt) s.move(0, 0), centred = centred && s.x == 0 && s.y == 0;
    CHECK(centred);
    if (!s.noise && !s.recoil) CHECK(std::abs(s.yaw - yaw) < 1e-3 && std::abs(s.pitch - pitch) < 1e-3);
}

void test_stick_model() {
    StickModel m;
    m.radial = false, m.deadzone = 0.15, m.curve = 2.25, m.yaw_speed = 84.5, m.pitch_speed = -90, m.lag = 3;
    StickModel back;
    CHECK(back.parse(m.serialize()));
    CHECK(!back.radial && back.deadzone == 0.15 && back.curve == 2.25 && back.yaw_speed == 84.5 && back.pitch_speed == -90 &&
          back.lag == 3);
    CHECK(!back.parse("round,0.1,1,100,100,2") && !back.parse("") && !back.parse("radial,0.1,1"));
    // The deflection for a speed turns the camera at that speed, axial and radial, inverted pitch included.
    for (bool radial : {false, true}) {
        m.radial = radial;
        for (double yaw : {-60.0, 5.0, 30.0})
            for (double pitch : {-20.0, 0.0, 45.0}) {
                float x, y;
                m.deflection(yaw, pitch, x, y);
                double ty, tp;
                m.speeds(x, y, ty, tp);
                CHECK(std::abs(ty - yaw) < 1e-3 && std::abs(tp - pitch) < 1e-3);
            }
    }
}

void test_stick_fallback_not_when_writes_last() {
    StickSim s;
    for (int i = 0; i < 200; ++i) s.frame(0.3, 1.0);
    CHECK(!s.fb.active());
    CHECK(std::abs(s.yaw - 200.0) < 1e-6);
    CHECK(s.x == 0 && s.y == 0);
}

void test_stick_fallback_not_at_pitch_limit() {
    StickSim s;
    for (int i = 0; i < 200; ++i) {
        s.frame(1.0, 0.2);
        s.pitch = std::min(s.pitch, 30.0);  // the game's pitch limit stops the pitch part of each write
    }
    CHECK(!s.fb.active());
    for (int i = 0; i < 100; ++i) {
        s.frame(1.0, 0);  // pitch only, against the limit: no verdict
        s.pitch = std::min(s.pitch, 30.0);
    }
    CHECK(!s.fb.active());
}

void test_stick_fallback_starts_the_right_way() {
    StickSim s;
    s.undoes = true;
    for (int i = 0; i < StickFallback::kUndoneFrames; ++i) s.frame(0, 0.5);
    CHECK(!s.fb.active());
    s.frame(0, 0.5);  // the last undone write is judged on the next frame
    CHECK(s.fb.active());
    // Before anything is learnt (the default model), the camera already turns the gyro's way.
    const double yaw = s.yaw, pitch = s.pitch;
    for (int i = 0; i < 60; ++i) s.frame(0.2, 0.5);
    CHECK(s.yaw - yaw > 10 && s.pitch - pitch > 3);
}

void test_stick_fallback_ends_when_writes_last_again() {
    StickSim s;
    s.undoes = true;
    for (int i = 0; i < 20; ++i) s.frame(0, 0.5);
    CHECK(s.fb.active());
    s.undoes = false;
    for (int i = 0; i < StickFallback::kProbeFrames + 2; ++i) s.frame(0, 0.5);
    CHECK(!s.fb.active());
}

void test_stick_fallback_ends_on_another_camera() {
    StickSim s;
    s.undoes = true;
    for (int i = 0; i < 20; ++i) s.frame(0, 0.5);
    CHECK(s.fb.active());
    s.camera = 2;
    s.frame(0, 0.5);
    CHECK(!s.fb.active());
}

void test_stick_fallback_gives_up_without_effect() {
    StickSim s;
    s.undoes = true;
    s.reads_stick = false;
    for (int i = 0; i < 20; ++i) s.frame(0, 2.0);
    CHECK(s.fb.active());
    for (int i = 0; i < StickFallback::kEffectFrames + 10; ++i) s.frame(0, 2.0);
    CHECK(!s.fb.active());
    for (int i = 0; i < 100; ++i) s.frame(0, 2.0);  // not again on that camera
    CHECK(!s.fb.active());
    s.camera = 2;  // another camera: detection again
    for (int i = 0; i < StickFallback::kUndoneFrames + 2; ++i) s.frame(0, 2.0);
    CHECK(s.fb.active());
}

void test_stick_fallback_leaves_player_stick_alone() {
    StickSim s;
    s.undoes = true;
    for (int i = 0; i < 40; ++i) s.frame(0, 0.5);
    for (int i = 0; i < 30; ++i) s.frame(0, 0);
    CHECK(s.fb.active());
    const double before = s.yaw;
    for (int i = 0; i < 30; ++i) s.frame(0, 0, true, 2.0);  // the player turns 60 deg with the stick, gyro still
    CHECK(s.fb.active());
    CHECK(s.x == 0);  // the fallback doesn't push back
    CHECK(std::abs(s.yaw - before - 60) < 1e-6);
}

void test_stick_fallback_forced() {
    StickSim s = forced_sim();
    s.frame(0, 0);
    CHECK(s.fb.active());
    for (int i = 0; i < 300; ++i) s.frame(0, 0.5);
    CHECK(s.fb.active());  // no probe write ends it, though the game keeps the writes
    s.fb.force(false);
    CHECK(!s.fb.active());
}

// No camera hook (EngineFrame::no_camera): the gyro always goes to the stick, never to the camera, from the first
// gameplay frame; off out of gameplay; a camera hook ends it.
void test_stick_fallback_blind() {
    std::puts("stick fallback: blind without a camera hook");
    StickFallback fb;
    uint64_t t = 1;
    float x = 0, y = 0;
    fb.frame_blind(true, t);
    CHECK(fb.active() && fb.blind() && !fb.direct());
    bool turned_right = false, turned_up = false;
    for (int i = 0; i < 60; ++i) {
        t += 16'666'667;
        fb.frame_blind(true, t);
        CHECK(!fb.direct());
        fb.turn(0.5, 1.0, false);
        CHECK(fb.stick(x, y));
        turned_right = turned_right || x > 0;
        turned_up = turned_up || y > 0;
    }
    CHECK(turned_right && turned_up);
    // A still gyro: a centred stick (nothing owed builds up forever).
    for (int i = 0; i < 30; ++i) {
        t += 16'666'667;
        fb.frame_blind(true, t);
        CHECK(fb.stick(x, y));
    }
    CHECK(x == 0 && y == 0);
    fb.frame_blind(false, t);  // menus, pause, no focus
    CHECK(!fb.active() && fb.direct() && !fb.stick(x, y));
    fb.frame_blind(true, t);
    CHECK(fb.active());
    fb.frame(true, 0, 0, 1, false, t);  // the engine has a camera hook now: detection as usual
    CHECK(!fb.active() && !fb.blind());
}

void test_stick_fallback_learns_linear_axial() {
    StickSim s = forced_sim();
    check_learns(s, 30, 0.12, "linear axial, dead zone 0.12, 81 deg/s");
    CHECK(!s.fb.model().radial && std::abs(s.fb.model().deadzone - 0.12) < 0.04);
}

void test_stick_fallback_learns_curved_radial_late() {
    // Like id Tech's: radial dead zone, power curve, slower pitch, two frames late and smoothed.
    StickSim s = forced_sim();
    s.radial = true, s.deadzone = 0.2, s.gamma = 2.2, s.speed = 85, s.speed_pitch = 70, s.lag = 2, s.smooth = 0.08;
    check_learns(s, 40, 0.2, "radial, dead zone 0.2, curve 2.2, 85/70 deg/s, 2 frames late, smoothed");
    CHECK(s.fb.model().radial);
}

void test_stick_fallback_learns_fast_axial() {
    // Like UE's default: axial dead zone 0.25, linear, fast.
    StickSim s = forced_sim();
    s.deadzone = 0.25, s.speed = 220, s.speed_pitch = 150, s.lag = 1;
    check_learns(s, 30, 0.12, "axial, dead zone 0.25, 220/150 deg/s, 1 frame late");
}

void test_stick_fallback_learns_with_noise_and_recoil() {
    // Motion that isn't the stick's (noise, a recoil kick every second) doesn't throw the model off.
    StickSim s = forced_sim();
    s.noise = 0.05, s.recoil = 1.5;
    check_learns(s, 40, 0.12, "linear axial with camera noise and recoil");
}

void test_stick_fallback_edge_acceleration() {
    // id Tech-like: radial, curved, and the speed triples after a second at the edge (90%): the model is learnt from
    // the rest, and no runaway or drift comes from the edge.
    StickSim s = forced_sim();
    s.radial = true, s.deadzone = 0.2, s.gamma = 2, s.speed = 90, s.speed_pitch = 90, s.edge = 0.9, s.edge_gain = 2;
    check_learns(s, 40, 0.15, "radial, curve 2, edge acceleration");
}

void test_stick_fallback_high_frame_rate() {
    // 144 frames/s with uneven frame times, the stick read 2 frames late.
    StickSim s = forced_sim();
    s.fps = 144, s.jitter = 0.3, s.dt = 1.0 / 144, s.lag = 2;
    check_learns(s, 30, 0.1, "linear axial at 144 frames/s, uneven, 2 frames late");
}

void test_stick_fallback_learns_inverted_pitch() {
    StickSim s = forced_sim();
    s.inverted = true;
    check_learns(s, 30, 0.12, "linear axial, inverted pitch");
    CHECK(s.fb.model().pitch_speed < 0 && s.fb.model().yaw_speed > 0);
}

// Data no game's stick gives (10 deg/s at full stick: in TNO, a game that didn't take the deflection) never becomes the
// model, and isn't saved.
void test_stick_fallback_rejects_implausible() {
    StickSim s = forced_sim();
    s.speed = s.speed_pitch = 10;
    aim(s, 30);
    std::string text;
    CHECK(!s.fb.learnt() && !s.fb.take_save(text));
    StickModel m;
    CHECK(!m.parse("radial,0.350,1.00,7.0,-5.0,0"));
}

void test_stick_fallback_learns_lag() {
    StickSim s = forced_sim();
    s.lag = 4;
    aim(s, 30);
    CHECK(s.fb.learnt() && std::abs(s.fb.model().lag - 4) <= 1);
}

void test_stick_fallback_pitch_limit() {
    // The pitch stops at +-6 degrees: pushing into the limit isn't a slow stick.
    StickSim s = forced_sim();
    s.pitch_limit = 6;
    aim(s, 40);
    CHECK(s.fb.learnt());
    std::printf("stick fallback, pitch limit: %s\n", s.fb.model().describe().c_str());
    CHECK(std::abs(s.fb.model().pitch_speed / 81 - 1) < 0.25);
    CHECK(std::abs(yaw_ratio(s, 30) - 1) < 0.12);
}

// A turn slower than the smallest deflection past the dead zone still turns the camera (pulses), and about right.
void test_stick_fallback_slow_aim() {
    StickSim s = forced_sim();
    aim(s, 30);
    const double yaw = s.yaw;
    for (int i = 0; i < 600; ++i) s.frame(0, 0.01);  // 0.6 deg/s for 10 s
    CHECK(std::abs(s.yaw - yaw - 6) < 1);
}

// The model saved for this game is used from the start: right speed at once, before anything is learnt.
void test_stick_fallback_restored_model() {
    StickSim s = forced_sim();
    s.deadzone = 0.25, s.speed = 220, s.speed_pitch = 150;
    CHECK(s.fb.restore("axial,0.250,1.00,220.0,150.0,0"));
    s.frame(0, 0);
    CHECK(!s.fb.learnt());
    CHECK(std::abs(yaw_ratio(s, 30) - 1) < 0.05);
    // Once learnt, it is offered for saving once, then again only when it changes.
    aim(s, 30);
    std::string text;
    if (s.fb.learnt() && s.fb.take_save(text)) {
        StickModel m;
        CHECK(m.parse(text));
    }
    CHECK(!s.fb.take_save(text));
}

// Without a saved model, the default turns the camera the right way in a game unlike it (slow, steep, late).
void test_stick_fallback_default_model_any_game() {
    StickSim s = forced_sim();
    s.radial = true, s.deadzone = 0.3, s.gamma = 2.5, s.speed = 60, s.speed_pitch = 60, s.lag = 3;
    s.frame(0, 0);
    const double yaw = s.yaw;
    for (int i = 0; i < 60; ++i) s.frame(0, 1.0);
    CHECK(s.yaw - yaw > 5);
}

// Stick camera: frames of 1/60 s; returns the yaw and pitch turned over them.
struct StickRun {
    StickCamera cam;
    uint64_t t = 1'000'000'000;
    double pitch = 0, yaw = 0;
    explicit StickRun(StickCamera::Settings s = {}) { cam.configure(s); }
    void frames(int n, float x, float y, uint64_t step_ns = 16'666'667) {
        for (int i = 0; i < n; ++i) {
            double p = 0, w = 0;
            cam.turn(x, y, t, p, w);
            pitch += p, yaw += w;
            t += step_ns;
        }
    }
};

StickCamera::Settings linear_no_edge() {
    StickCamera::Settings s;
    s.curve = 1;
    s.edge = 1;
    s.edge_speed = s.speed;
    return s;
}

void test_stick_camera_deadzone() {
    StickRun r;
    r.frames(120, 0.09f, 0.0f);
    CHECK(r.yaw == 0 && r.pitch == 0);
}

void test_stick_camera_full_speed() {
    StickRun r(linear_no_edge());
    r.frames(1, 1, 0);  // the first frame has no frame time
    CHECK(r.yaw == 0);
    r.frames(60, 1, 0);
    CHECK(std::abs(r.yaw - 360) < 1e-3);
    CHECK(r.pitch == 0);
    StickRun up(linear_no_edge());
    up.frames(61, 0, 1);
    CHECK(std::abs(up.pitch - 200) < 1e-3);
}

void test_stick_camera_diagonal() {
    StickRun axis(linear_no_edge()), diagonal(linear_no_edge());
    axis.frames(61, 1, 0);
    diagonal.frames(61, 1, 1);  // a square gate: past 1 on the diagonal
    CHECK(diagonal.yaw < axis.yaw);
    CHECK(std::abs(diagonal.yaw - 360 / std::sqrt(2.0)) < 1e-3);
}

void test_stick_camera_curve() {
    StickRun r;  // curve 2, deadzone 10 %: half way past the deadzone gives a quarter of the speed
    r.frames(61, 0.55f, 0);
    CHECK(std::abs(r.yaw - 90) < 0.5);
}

void test_stick_camera_edge_ramp() {
    StickCamera::Settings s = linear_no_edge();
    s.edge = 0.9;
    s.edge_speed = 720;
    s.edge_ramp_ms = 400;
    StickRun r(s);
    r.frames(1, 1, 0);
    r.frames(30, 1, 0);  // 0.5 s: past the ramp
    const double before = r.yaw;
    r.frames(60, 1, 0);  // one second at the edge speed
    CHECK(std::abs(r.yaw - before - 720) < 1e-3);
    CHECK(before > 180 && before < 360);  // ramping up during the first half second
    // Leaving the edge starts the ramp over.
    r.frames(1, 0.5f, 0);
    const double back = r.yaw;
    r.frames(1, 1, 0);
    CHECK(r.yaw - back < 7);  // about 360 / 60, not 720 / 60
}

void test_stick_camera_dt_cap() {
    StickRun r(linear_no_edge());
    r.frames(1, 1, 0);
    r.frames(1, 1, 0, 2'000'000'000);  // one frame, then a 2 s stall: the next frame turns at most kMaxDt
    r.frames(1, 1, 0);
    CHECK(std::abs(r.yaw - 360 * (1.0 / 60 + StickCamera::kMaxDt)) < 1e-3);
    r.cam.reset();
    const double before = r.yaw;
    r.frames(1, 1, 0);
    CHECK(r.yaw == before);  // after a reset, no frame time again
}

void test_stick_camera_invert_y() {
    StickCamera::Settings s = linear_no_edge();
    s.invert_y = true;
    StickRun r(s);
    r.frames(61, 0, 1);
    CHECK(std::abs(r.pitch + 200) < 1e-3);
}

// Sensor-time pacing: a controller turning at `rate` deg/s (yaw) whose reports come at uneven intervals (as the
// DualSense's over USB, MODLOG 2026-10-29) and are read at each frame, all stamped with the read time as SDL does;
// GyroLib's turn for a frame = rate times the sensor time of the reports read in it.
struct PacerRun {
    SensorPacer pacer;
    uint64_t sensor = 1'000'000'000, next_report = 0;  // host time of the next report's measurement
    uint64_t now = 5'000'000'000, last_read_sensor = 0;
    size_t step = 0;
    double rate = 0, input = 0, output = 0;
    std::vector<double> turns;  // yaw per frame
    explicit PacerRun(SensorPacer::Settings s = {}) {
        pacer.configure(s);
        next_report = now;
    }
    // One frame of frame_ns: reports measured up to now - latency are read.
    void frame(uint64_t frame_ns, bool burst_output = false) {
        static constexpr uint64_t kIntervals[] = {1'300'000, 1'900'000, 2'500'000, 3'100'000, 1'300'000, 2'500'000, 1'900'000, 3'800'000};
        static constexpr uint64_t kLatency[] = {300'000, 800'000, 1'500'000, 400'000, 2'200'000};
        now += frame_ns;
        double gl = 0;
        while (next_report + kLatency[step % 5] <= now) {
            const uint64_t interval = kIntervals[step % 8];
            sensor += interval;
            next_report += interval;
            pacer.sample(sensor, now - 13'000);
            if (last_read_sensor) gl += rate * double(sensor - last_read_sensor) * 1e-9;
            last_read_sensor = sensor;
            ++step;
        }
        if (burst_output) gl += 1.0;
        input += gl;
        double pitch = 0, yaw = gl;
        pacer.frame(now, pitch, yaw);
        output += yaw;
        turns.push_back(yaw);
    }
    void frames(int n, uint64_t frame_ns) {
        for (int i = 0; i < n; ++i) frame(frame_ns);
    }
    // Largest relative deviation of the last n turns from rate * frame time.
    double worst(int n, uint64_t frame_ns) const {
        double w = 0;
        const double expect = rate * double(frame_ns) * 1e-9;
        for (size_t i = turns.size() - n; i < turns.size(); ++i) w = std::max(w, std::abs(turns[i] / expect - 1));
        return w;
    }
};

void test_pacer_steady_turn_even() {
    PacerRun off(SensorPacer::Settings{false, -1});
    off.rate = 100;
    off.frames(300, 10'000'000);
    PacerRun r;
    r.rate = 100;
    r.frames(300, 10'000'000);
    // Without pacing the turn per frame follows the reports read (here up to ~30% off); paced, within a few percent.
    std::printf("sensor pacing, 100 deg/s at 100 FPS: worst frame off by %.0f%% unpaced, %.1f%% paced (delay %.2f ms)\n",
                100 * off.worst(200, 10'000'000), 100 * r.worst(200, 10'000'000), r.pacer.delay_ms());
    CHECK(off.worst(200, 10'000'000) > 0.15);
    CHECK(r.worst(200, 10'000'000) < 0.05);
    CHECK(r.pacer.delay_ms() > 0 && r.pacer.delay_ms() < 4);
}

void test_pacer_variable_frames() {
    // VRR between 165 and 100 FPS: each frame turns by its own length.
    PacerRun r;
    r.rate = 200;
    r.frames(200, 8'000'000);
    double w = 0;
    for (int i = 0; i < 300; ++i) {
        const uint64_t frame_ns = (i % 3 == 0) ? 6'000'000 : (i % 3 == 1) ? 10'000'000 : 7'500'000;
        r.frame(frame_ns);
        if (i > 10) w = std::max(w, std::abs(r.turns.back() / (r.rate * double(frame_ns) * 1e-9) - 1));
    }
    std::printf("sensor pacing, 200 deg/s at 100-165 FPS: worst frame off by %.1f%%\n", 100 * w);
    CHECK(w < 0.12);
}

void test_pacer_stop_keeps_total_no_step_back() {
    PacerRun r;
    r.rate = 150;
    r.frames(200, 10'000'000);
    r.rate = 0;
    r.frames(50, 10'000'000);
    bool back = false;
    for (double t : r.turns) back |= t < 0;
    CHECK(!back);
    // Whatever an extrapolation overshot stays owed (paid back on the next motion): at most a few ms of motion.
    CHECK(std::abs(r.output - r.input) < 150 * 0.005);
    // Turning again the same way, the overshoot is paid back without a stop.
    r.rate = 150;
    r.frames(100, 10'000'000);
    CHECK(std::abs(r.output - r.input) < 150 * 0.003);
    bool stalled = false;
    for (size_t i = r.turns.size() - 90; i < r.turns.size(); ++i) stalled |= r.turns[i] <= 0;
    CHECK(!stalled);
}

void test_pacer_reverse() {
    PacerRun r;
    r.rate = 150;
    r.frames(100, 10'000'000);
    r.rate = -150;
    r.frames(100, 10'000'000);
    CHECK(r.worst(80, 10'000'000) < 0.05);
    CHECK(std::abs(r.output - r.input) < 150 * 0.003);
}

void test_pacer_burst_passthrough_and_off() {
    // GyroLib's turn without new reports (its burst delivery): what is owed goes out with it, and GyroLib's turn goes
    // through unpaced for a second.
    PacerRun r;
    r.rate = 100;
    r.frames(50, 10'000'000);
    double pitch = 0.5, yaw = 1.0;
    r.now += 100'000;
    r.pacer.frame(r.now, pitch, yaw);
    r.input += 1.0;
    r.output += yaw;
    // Owed (either way: the delay behind, an extrapolation ahead): at most a couple of ms of motion.
    CHECK(pitch == 0.5 && std::abs(yaw - 1.0) < 100 * 0.002);
    CHECK(std::abs(r.output - r.input) < 1e-9);
    const double before = r.input;
    r.frame(10'000'000);
    CHECK(std::abs(r.turns.back() - (r.input - before)) < 1e-12);
    uint32_t paced = 0, extrapolated = 0, bursts = 0, stalls = 0;
    r.pacer.take_counts(paced, extrapolated, bursts, stalls);
    CHECK(paced == 50 && bursts == 2);
    // A second after the last of them, pacing again.
    r.frames(110, 10'000'000);
    r.pacer.take_counts(paced, extrapolated, bursts, stalls);
    CHECK(paced > 0 && bursts > 90);
    // Off: untouched.
    SensorPacer off;
    off.configure({false, -1});
    off.sample(1'000'000, 2'000'000);
    pitch = 0.25, yaw = -2;
    off.frame(3'000'000, pitch, yaw);
    CHECK(pitch == 0.25 && yaw == -2);
}

void test_pacer_stall_at_once() {
    // A 140 ms stall in the middle of a turn: its turn (and what was owed) goes out in that frame, not spread over the
    // next ones (MODLOG 2026-10-29: a stall's 13.5 deg took another 100+ ms to catch up).
    PacerRun r;
    r.rate = 100;
    r.frames(100, 6'300'000);
    r.frame(140'000'000);
    CHECK(std::abs(r.output - r.input) < 1e-9);
    uint32_t paced = 0, extrapolated = 0, bursts = 0, stalls = 0;
    r.pacer.take_counts(paced, extrapolated, bursts, stalls);
    CHECK(stalls == 1 && bursts == 0);
    r.frames(100, 6'300'000);
    CHECK(r.worst(90, 6'300'000) < 0.06);
}

void test_pacer_fixed_delay() {
    SensorPacer::Settings s;
    s.delay_ns = 4'000'000;
    PacerRun r(s);
    r.rate = 100;
    r.frames(300, 10'000'000);
    CHECK(std::abs(r.pacer.delay_ms() - 4) < 1e-9);
    CHECK(r.worst(200, 10'000'000) < 0.05);
}

// Zoom compensation (zoom.hpp), with Mass Effect 3 LE's fields of view (MODLOG 2026-10-30): 100 degrees horizontal
// unzoomed at 16:9, 10 through the Black Widow's scope, 39.43 aiming the Shuriken, 120 sprinting.

// The scale is GyroLib's tan ratio, wider fields of view included, and the same for horizontal and vertical FOVs.
void test_zoom_scale() {
    std::puts("zoom: the scale is the tan ratio, the same in horizontal and vertical degrees");
    CHECK(std::abs(zoom_scale(10, 100) - 0.0734) < 1e-3);
    CHECK(std::abs(zoom_scale(39.43, 100) - 0.300) < 1e-3);
    CHECK(std::abs(zoom_scale(120, 100) - 1.453) < 1e-3);
    CHECK(zoom_scale(100, 100) == 1);
    CHECK(zoom_scale(10, 0) == 1);
    CHECK(zoom_scale(0, 100) == 1);
    for (const double aspect : {4.0 / 3, 16.0 / 9, 21.0 / 9})
        for (const double f : {5.0, 10.0, 39.43, 90.0, 120.0})
            CHECK(std::abs(zoom_scale(fov::vertical_from_horizontal(f, aspect), fov::vertical_from_horizontal(100, aspect)) -
                           zoom_scale(f, 100)) < 1e-9);
}

// ZoomStrength: the scale raised to strength / 100, and the FOV reported for it gives GyroLib exactly that scale.
void test_zoom_strength() {
    std::puts("zoom: ZoomStrength raises the scale; the reported FOV gives GyroLib that scale");
    const double full = zoom_scale(10, 100);
    CHECK(std::abs(zoom_scale(10, 100, 0.75) - std::pow(full, 0.75)) < 1e-12);
    CHECK(zoom_scale(10, 100, 0) == 1);
    CHECK(zoom_scale(120, 100, 0) == 1);
    // How much faster a zoom feels than at 100: its magnification^(1 - strength); 75%: the scope 1.92x, a 4x zoom 1.41x.
    CHECK(std::abs(zoom_scale(10, 100, 0.75) / full - 1.92) < 0.01);
    CHECK(std::abs(std::pow(0.25, 0.75) / 0.25 - 1.414) < 0.001);
    for (const double strength : {0.0, 0.5, 0.75, 1.0, 1.5})
        for (const double f : {5.0, 10.0, 39.43, 100.0, 120.0, 150.0}) {
            const double reported = strengthened_fov(f, 100, strength);
            CHECK(std::abs(std::tan(reported * kHalfDegree) / std::tan(100 * kHalfDegree) - zoom_scale(f, 100, strength)) <
                  1e-9);
            CHECK(fov::is_valid(reported));
        }
    CHECK(std::abs(strengthened_fov(10, 100, 1) - 10) < 1e-9);
    CHECK(std::abs(fov_for_scale(60, 1) - 60) < 1e-9);
    CHECK(fov_for_scale(60, 0) == 0.1);  // clamped inside what GyroLib takes
}

// Manual zoom levels: hold and toggle, cycling with wrap, aiming ends on release and on lost gameplay.
void test_zoom_levels_basic() {
    std::puts("zoom: manual levels, hold and toggle, cycle and wrap, reset on release and on lost gameplay");
    ZoomLevels z;
    z.configure({3, false, false});
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(true, true, false, false) == 1);   // aim held
    CHECK(z.update(true, true, true, false) == 2);    // cycle press
    CHECK(z.update(true, true, true, false) == 2);    // still held: no new press
    CHECK(z.update(true, true, false, false) == 2);
    CHECK(z.update(true, true, true, false) == 3);
    CHECK(z.update(true, true, false, false) == 3);
    CHECK(z.update(true, true, true, false) == 1);    // after the last, back to the first
    CHECK(z.update(true, false, false, false) == 0);  // released
    CHECK(z.update(true, true, false, false) == 1);   // a new aim starts at the first level
    CHECK(z.update(false, true, false, false) == 0);  // gameplay interrupted
    CHECK(z.update(true, false, true, false) == 0);   // cycle without aiming does nothing
    CHECK(z.update(true, false, false, false) == 0);

    z.configure({2, true, false});  // toggle
    CHECK(z.update(true, true, false, false) == 1);
    CHECK(z.update(true, false, false, false) == 1);  // stays on after the release
    CHECK(z.update(true, false, true, false) == 2);
    CHECK(z.update(true, false, false, false) == 2);
    CHECK(z.update(true, true, false, false) == 0);   // the second press turns it off
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(true, true, false, false) == 1);
    CHECK(z.update(false, false, false, false) == 0);  // a toggle is dropped when gameplay is interrupted
    CHECK(z.update(true, false, false, false) == 0);

    z.configure({1, false, false});  // one level: the cycle button changes nothing
    CHECK(z.update(true, true, false, false) == 1);
    CHECK(z.update(true, true, true, false) == 1);
    z.configure({0, false, false});  // no levels: off
    CHECK(z.update(true, true, false, false) == 0);
}

// ZoomRememberLevel and ZoomResetButtons.
void test_zoom_levels_remember() {
    std::puts("zoom: the remembered level, and the reset button while aiming and while not");
    ZoomLevels z;
    z.configure({3, false, true});
    CHECK(z.update(true, true, false, false) == 1);
    CHECK(z.update(true, true, true, false) == 2);
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(true, true, false, false) == 2);   // the next aim starts where the last one ended
    CHECK(z.update(true, true, false, true) == 1);    // reset while aiming: back to the first level now
    CHECK(z.update(true, true, true, false) == 2);
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(true, false, false, true) == 0);   // reset while not aiming (a weapon switch)
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(true, true, false, false) == 1);   // ... the next aim starts at the first level
    CHECK(z.update(true, false, false, false) == 0);
    CHECK(z.update(false, false, false, true) == 0);  // a reset button outside gameplay (menus) doesn't count
    z.update(true, true, false, false);
    z.update(true, true, true, false);
    CHECK(z.level() == 2);
    z.update(true, false, false, false);
    CHECK(z.update(false, false, false, true) == 0);
    CHECK(z.update(true, true, false, false) == 2);

    z.configure({3, false, false});  // without remember, a reset only matters while aiming
    z.update(true, true, false, false);
    CHECK(z.update(true, true, true, false) == 2);
    CHECK(z.update(true, true, false, true) == 1);
}

// The manual zoom's ramp: linear in log-scale, every change takes the ramp's time, back to 1 the same way.
void test_zoom_ramp() {
    std::puts("zoom: the ramp reaches each target in ZoomRampMs, linear in log-scale");
    constexpr uint64_t ms = 1'000'000;
    ZoomRamp r;
    CHECK(r.value() == 1);
    CHECK(r.step(1, 0, 150 * ms) == 1);
    CHECK(std::abs(r.step(0.25, 10 * ms, 150 * ms) - 1) < 1e-12);           // the change starts the ramp
    CHECK(std::abs(r.step(0.25, 85 * ms, 150 * ms) - 0.5) < 1e-9);          // halfway in log-scale: sqrt(0.25)
    CHECK(std::abs(r.step(0.25, 160 * ms, 150 * ms) - 0.25) < 1e-12);
    CHECK(std::abs(r.step(0.25, 500 * ms, 150 * ms) - 0.25) < 1e-12);
    CHECK(std::abs(r.step(1, 600 * ms, 150 * ms) - 0.25) < 1e-12);          // and back
    CHECK(std::abs(r.step(1, 675 * ms, 150 * ms) - 0.5) < 1e-9);
    CHECK(std::abs(r.step(0.25, 675 * ms, 150 * ms) - 0.5) < 1e-9);         // turned around mid-ramp: from there
    CHECK(std::abs(r.step(0.25, 825 * ms, 150 * ms) - 0.25) < 1e-12);
    CHECK(std::abs(r.step(1, 900 * ms, 0) - 1) < 1e-12);                    // no ramp: at once
}

void test_unity_legacy_keys() {
    using gyro4all::unity::legacy_key_button;
    CHECK(legacy_key_button(330) == 0x1000);  // JoystickButton0 = A
    CHECK(legacy_key_button(331) == 0x2000);  // JoystickButton1 = B
    CHECK(legacy_key_button(337) == 0x0010);  // JoystickButton7 = Start
    CHECK(legacy_key_button(339) == 0x0080);  // JoystickButton9 = right stick
    CHECK(legacy_key_button(340) == 0);       // no button 10 on an Xbox controller
    CHECK(legacy_key_button(350) == 0x1000);  // Joystick1Button0 = A
    CHECK(legacy_key_button(370) == 0);       // Joystick2Button0: another joystick
    CHECK(legacy_key_button(329) == 0);       // a mouse button
    CHECK(legacy_key_button(13) == 0);        // Return
}

void test_unity_quaternions() {
    using namespace gyro4all::unity;
    auto near_angle = [](double a, double b) { return std::abs(wrap180(a - b)) < 1e-9; };
    ViewAngles a = view_angles(from_euler(0, 0));
    CHECK(near_angle(a.pitch, 0) && near_angle(a.yaw, 0));
    a = view_angles(from_euler(30, 40));
    CHECK(near_angle(a.pitch, -30) && near_angle(a.yaw, 40));  // Unity +X looks down; the engine interface pitches up
    a = view_angles(add_turn(from_euler(0, 170), 0, 25));
    CHECK(near_angle(a.pitch, 0) && near_angle(a.yaw, -165));
    a = view_angles(add_turn(from_euler(0, 0), 20, 0));
    CHECK(near_angle(a.pitch, 20) && near_angle(a.yaw, 0));
    // A body's yaw around an up: the world's for an upright frame, the planet's for a body on its side.
    a = view_angles(add_yaw_about(from_euler(0, 170), up_of(Quaternion{}), 25));
    CHECK(near_angle(a.pitch, 0) && near_angle(a.yaw, -165));
    const Quaternion tilted = from_euler(0, 0, 90);  // up is the world's -X: a body on the side of a planet
    const Vector3 tilted_up = up_of(tilted);
    CHECK(std::abs(tilted_up.x + 1) < 1e-9 && std::abs(tilted_up.y) < 1e-9 && std::abs(tilted_up.z) < 1e-9);
    const Quaternion yawed = add_yaw_about(tilted, tilted_up, 30);
    const Vector3 u1 = up_of(yawed);
    CHECK(std::abs(u1.x - tilted_up.x) < 1e-9 && std::abs(u1.y - tilted_up.y) < 1e-9 && std::abs(u1.z - tilted_up.z) < 1e-9);
    CHECK(std::abs(yaw_between_about(tilted, yawed, tilted_up) - 30) < 1e-9);
    CHECK(std::abs(yaw_between_about(yawed, tilted, tilted_up) + 30) < 1e-9);
    // A pitched head under an upright parent (PowerWash Simulator's HeadTurn): the parent's up gives a plain world yaw, and
    // the pitch it carries adds no yaw.
    const Quaternion head = from_euler(-20, 40);
    a = view_angles(add_yaw_about(head, up_of(Quaternion{}), 10));
    CHECK(near_angle(a.pitch, 20) && near_angle(a.yaw, 50));
    CHECK(std::abs(yaw_between_about(head, add_yaw_about(head, up_of(Quaternion{}), 10), up_of(Quaternion{})) - 10) < 1e-9);
    CHECK(std::abs(yaw_between_about(from_euler(0, 40), head, up_of(Quaternion{}))) < 1e-9);
    // A root body (Outer Wilds' Player_Body) turns around its own up: the same as a local yaw, q * Ry.
    for (const Quaternion& q : {tilted, head, from_euler(33, -120, 47)}) {
        const Quaternion world = add_yaw_about(q, up_of(q), 17), local = normalize(multiply(q, from_euler(0, 17)));
        CHECK(std::abs(world.x * local.x + world.y * local.y + world.z * local.z + world.w * local.w) > 1 - 1e-12);
        CHECK(std::abs(yaw_between_about(q, local, up_of(local)) - 17) < 1e-9);
    }
    CHECK(yaw_up_from_parent(head, Quaternion{}));                     // a pitched head: its parent's up
    CHECK(yaw_up_from_parent(from_euler(0, 40), Quaternion{}));        // upright: either is the same
    CHECK(!yaw_up_from_parent(tilted, Quaternion{}));                  // on a planet's side, under a world-aligned frame
    CHECK(!yaw_up_from_parent(from_euler(-20, 40, 15), Quaternion{})); // rolled: not a pitch alone
    // The probe's angles in a body's frame: the world's for an upright body, the look's own for a tilted one.
    {
        const Quaternion upright = from_euler(0, 70), look = from_euler(-25, 15);  // the camera's turn on the body
        const ViewAngles world = view_angles(multiply(upright, look));
        const ViewAngles in_body = angles_in_body(upright, 70, multiply(upright, look));
        CHECK(near_angle(in_body.pitch, world.pitch) && near_angle(in_body.yaw, world.yaw));
        const ViewAngles own = view_angles(look);
        for (const Quaternion& body : {tilted, from_euler(33, -120, 47)}) {
            const ViewAngles a2 = angles_in_body(body, 10, multiply(body, look));
            CHECK(near_angle(a2.pitch, own.pitch) && near_angle(a2.yaw, own.yaw + 10));
            // The body yawed around its own up by 30 (body yaw added up to 40): the camera's yaw 30 more, pitch unchanged.
            const Quaternion turned = add_yaw_about(body, up_of(body), 30);
            const ViewAngles a3 = angles_in_body(turned, 10 + yaw_between_about(body, turned, up_of(turned)), multiply(turned, look));
            CHECK(near_angle(a3.pitch, own.pitch) && near_angle(a3.yaw, own.yaw + 40));
        }
    }
    const Quaternion combined = multiply(from_euler(10, 20, 30), from_euler(-5, 15, -10));
    CHECK(std::isfinite(combined.x) && std::isfinite(combined.y) && std::isfinite(combined.z) &&
          std::isfinite(combined.w));
}

void test_unity_turn_preserves_roll_and_levels() {
    using namespace gyro4all::unity;
    const Quaternion original = from_euler(15, 60, 25);
    const Quaternion turned = add_turn(original, 5, 10);
    const Quaternion expected = multiply(multiply(from_euler(0, 10), original), from_euler(-5, 0));
    const double dot = std::abs(turned.x * expected.x + turned.y * expected.y + turned.z * expected.z +
                                turned.w * expected.w);
    CHECK(dot > 1 - 1e-12);
    CHECK(std::abs(view_angles(level(original)).pitch) < 1e-9);
    CHECK(std::abs(wrap180(view_angles(level(original)).yaw - 60)) < 1e-9);
    const Quaternion almost_vertical = add_turn(from_euler(0, 0), 89.9, 30);
    CHECK(std::isfinite(almost_vertical.x) && std::isfinite(almost_vertical.y) && std::isfinite(almost_vertical.z) &&
          std::isfinite(almost_vertical.w));
}

void test_unity_profile() {
    using gyro4all::unity::load_unity_profile;
    const std::wstring missing = temp_profile(L"unity_missing");
    CHECK(!load_unity_profile(missing).enabled());
    CHECK(!load_unity_profile(missing).probe);
    const std::wstring path = temp_profile(L"unity_profile");
    write_file(path, "[Unity]\r\nObjectType=RoR2.CameraRigController\r\nCameraField=sceneCam\r\n"
                     "PitchYawPath=_cameraMode.DebugGetInstanceData(@).pitchYaw\r\n"
                     "PitchLimit=85\r\nProbe=1\r\n");
    const auto profile = load_unity_profile(path);
    CHECK(profile.enabled());
    CHECK(profile.object_type == "RoR2.CameraRigController" && profile.camera_field == "sceneCam");
    CHECK(profile.path.size() == 3 && profile.path[1] == "DebugGetInstanceData(@)" && profile.path[2] == "pitchYaw");
    CHECK(profile.pitch_limit == 85);
    CHECK(profile.probe);
    CHECK(profile.pitch_per_degree == -1 && profile.yaw_per_degree == 1);  // Unity's Euler convention by default
    // Two separate floats, with their own units; the camera is optional.
    const std::wstring floats = temp_profile(L"unity_floats");
    write_file(floats, "[Unity]\r\nObjectType=Look\r\nYawObjectType=Body\r\nPitchPath=_pitch\r\nYawPath=state.yaw\r\n"
                       "PitchPerDegree=0.0174533\r\nYawPerDegree=-1\r\n");
    const auto separate = load_unity_profile(floats);
    CHECK(separate.enabled() && separate.camera_field.empty() && separate.yaw_object_type == "Body");
    CHECK(separate.pitch_path.size() == 1 && separate.yaw_path.size() == 2 && separate.yaw_path[1] == "yaw");
    CHECK(std::abs(separate.pitch_per_degree - 0.0174533) < 1e-9 && separate.yaw_per_degree == -1);
    CHECK(!(separate == profile) && load_unity_profile(floats) == separate);
    CHECK(!separate.pitch_transform && !separate.yaw_from_camera);
    // A yaw relative to a body the game turns itself, through a LinkedList (Firewatch's camera mode stack).
    write_file(floats, "[Unity]\r\nObjectType=vgCameraController\r\nPitchPath=cameraModeStack[vgCameraModeInstance].localOffset.y\r\n"
                       "PitchPerDegree=1\r\nYawPath=cameraModeStack[vgCameraModeInstance].localOffset.x\r\nYawFromCamera=1\r\n");
    const auto relative = load_unity_profile(floats);
    CHECK(relative.enabled() && relative.yaw_from_camera && relative.yaw_path.size() == 3 &&
          relative.yaw_path[0] == "cameraModeStack[vgCameraModeInstance]");
    // The body's yaw with a pitch float the game copies to the camera only on look input (The Planet Crafter).
    write_file(floats, "[Unity]\r\nObjectType=SpaceCraft.PlayerLookable\r\nPitchPath=_pitch\r\nYawParent=1\r\n"
                       "PitchTransform=1\r\nPitchLimit=80\r\n");
    const auto body = load_unity_profile(floats);
    CHECK(body.enabled() && body.yaw_parent == 1 && body.pitch_transform && body.pitch_limit == 80);
    // One float alone, or a zero scale, doesn't make a usable view.
    write_file(floats, "[Unity]\r\nObjectType=Look\r\nPitchPath=_pitch\r\nYawPerDegree=0\r\n");
    CHECK(!load_unity_profile(floats).enabled());
    CHECK(load_unity_profile(floats).yaw_per_degree == 1);
    // Both axes on Transforms the player script holds, the game's Look action as the gate, its own unzoomed FOV (MOUSE).
    write_file(floats, "[Unity]\r\nObjectType=Player\r\nPitchObject=cameraRigController\r\nYawObject=characterController\r\n"
                       "Look=gameObject.activeInHierarchy, inputs._controls.m_Player_Look.enabled\r\n"
                       "ZoomReference=cameraFovController._baseVerticalFOV\r\n");
    const auto objects = load_unity_profile(floats);
    CHECK(objects.enabled() && objects.pitch_on_object() && objects.yaw_on_object() && objects.has_view());
    CHECK(objects.pitch_object.size() == 1 && objects.yaw_object[0] == "characterController");
    CHECK(objects.look.size() == 2 && !objects.look[0].negate && objects.look[1].path.size() == 4 &&
          objects.look[1].path[3] == "enabled");
    CHECK(objects.zoom_reference.size() == 2 && objects.zoom_reference_value == 0 &&
          objects.zoom_axis == gyro4all::unity::FovAxis::Auto);
    // A float wins over an object for the same axis; YawParent too; ZoomReference= may be a number.
    write_file(floats, "[Unity]\r\nObjectType=Look\r\nPitchPath=_pitch\r\nPitchObject=rig\r\nYawParent=1\r\nYawObject=body\r\n"
                       "ZoomReference=60\r\nZoomReferenceAxis=horizontal\r\nLook=!paused\r\n");
    const auto mixed = load_unity_profile(floats);
    CHECK(mixed.enabled() && !mixed.pitch_on_object() && !mixed.yaw_on_object() && mixed.zoom_reference_value == 60);
    CHECK(mixed.zoom_axis == gyro4all::unity::FovAxis::Horizontal);
    CHECK(mixed.look.size() == 1 && mixed.look[0].negate && mixed.look[0].path[0] == "paused");
    // An object alone for one axis is no view; a malformed Look= is dropped whole.
    write_file(floats, "[Unity]\r\nObjectType=Player\r\nPitchObject=cameraRigController\r\nLook=a..b, c\r\nZoomReference=500\r\n");
    const auto half = load_unity_profile(floats);
    CHECK(!half.enabled() && half.look.empty() && half.zoom_reference_value == 0 && half.zoom_reference.empty());
    std::vector<gyro4all::unity::LookCondition> look;
    CHECK(gyro4all::unity::parse_look("", look) && look.empty());
    CHECK(gyro4all::unity::parse_look(" a.b , ,! c ", look) && look.size() == 2 && look[1].negate && look[1].path[0] == "c");
    CHECK(!gyro4all::unity::parse_look("a b", look) && look.empty());
    CHECK(!gyro4all::unity::parse_look("!", look));
}

// ZoomReference='s axis: Camera.fieldOfView is vertical; MOUSE's _baseVerticalFOV holds 105, its horizontal at 16:9 (72.5).
void test_unity_zoom_reference_axis() {
    using namespace gyro4all::unity;
    std::puts("unity: ZoomReference's axis told from the camera's FOV, and the probe's pick of the unzoomed FOV");
    constexpr double wide = 16.0 / 9;
    const double v105 = gyro4all::fov::vertical_from_horizontal(105, wide);
    CHECK(std::abs(v105 - 72.45) < 0.1);
    CHECK(parse_fov_axis("Vertical") == FovAxis::Vertical && parse_fov_axis("h") == FovAxis::Horizontal &&
          parse_fov_axis("") == FovAxis::Auto && parse_fov_axis("diagonal") == FovAxis::Auto);
    CHECK(fov_axis_seen(105, v105, wide) == FovAxis::Horizontal);
    CHECK(fov_axis_seen(72.5, 72.5, wide) == FovAxis::Vertical);
    CHECK(fov_axis_seen(105, 40, wide) == FovAxis::Auto);  // zoomed: neither
    CHECK(fov_axis_seen(90, 90, 1.0) == FovAxis::Auto);    // a square window: both
    // Unzoomed frames decide it; zoomed ones don't count, and a few matching the other axis don't override it.
    FovAxisVote vote;
    for (int i = 0; i < 100; ++i) CHECK(vote.add(105, 40, wide) == FovAxis::Auto);
    for (int i = 0; i < 5; ++i) vote.add(105, 105, wide);
    for (int i = 0; i < FovAxisVote::kVotes - 1; ++i) CHECK(vote.add(105, v105, wide) == FovAxis::Auto);
    CHECK(vote.add(105, v105, wide) == FovAxis::Horizontal);
    CHECK(vote.add(105, 105, wide) == FovAxis::Horizontal);  // decided: stays
    vote.reset();
    CHECK(vote.decided() == FovAxis::Auto);
    for (int i = 0; i < FovAxisVote::kVotes; ++i) vote.add(60, 60, wide);
    CHECK(vote.decided() == FovAxis::Vertical);
    // The probe's pick: named for a FOV, not a zoom or a speed, equal to the camera's in one axis; base and the like first.
    const auto mouse = fov_reference_match("_baseVerticalFOV", 105, v105, wide);
    CHECK(mouse && mouse->axis == FovAxis::Horizontal && mouse->score == 3);
    const auto plain = fov_reference_match("settings.fov", v105, v105, wide);
    CHECK(plain && plain->axis == FovAxis::Vertical && plain->score == 1);
    CHECK(!fov_reference_match("_zoomFov", 105, v105, wide) && !fov_reference_match("currentFov", v105, v105, wide));
    CHECK(!fov_reference_match("fovLerpSpeed", 105, v105, wide) && !fov_reference_match("_baseHeight", 105, v105, wide));
    CHECK(!fov_reference_match("_baseFov", 90, v105, wide));  // not the camera's in either axis
}

void test_unity_aim_check() {
    using gyro4all::unity::AimCheck;
    using gyro4all::unity::look_motion;
    AimCheck aim;
    // Three windows of look input with a still camera park it; two answering windows bring it back.
    aim.window(0, true);
    aim.window(0, true);
    CHECK(!aim.unresponsive());
    aim.window(0, true);
    CHECK(aim.unresponsive());
    aim.window(20, true);
    CHECK(aim.unresponsive());  // one answering window isn't enough yet
    aim.window(20, true);
    CHECK(!aim.unresponsive());
    // Skipped windows (menus, pause, the mod's own camera writes) keep the streaks as they are.
    aim.window(0, true);
    aim.window(0, true);
    aim.window(0, false);
    aim.window(0, true);
    CHECK(aim.unresponsive());
    aim.reset();
    CHECK(!aim.unresponsive());
    // Look input turning a still camera answers; a pan that goes on by itself at the same speed doesn't, however far it goes.
    CHECK(look_motion(30, 1000, 0) == 30);
    CHECK(look_motion(30, 1000, 10) == 30);
    CHECK(look_motion(10, 1000, 10) == 0);
    CHECK(look_motion(40, 2000, 15) == 0);
    CHECK(look_motion(5, 0, 0) == 0);
}

void test_unity_pitch_clamp() {
    using gyro4all::unity::limit_pitch_turn;
    using gyro4all::unity::PitchClamp;
    PitchClamp clamp;
    // A write the game keeps teaches nothing, however far it goes.
    for (int i = 0; i < 10; ++i) CHECK(!clamp.observe(70, 70));
    CHECK(clamp.up == 89);
    // Pulled back to the same pitch frame after frame: the limit. One frame isn't enough (a scripted moment).
    CHECK(!clamp.observe(80, 60));
    CHECK(!clamp.observe(80, 60.2));
    CHECK(clamp.observe(80, 60.1));
    CHECK(std::abs(clamp.up - 60.1) < 1e-9 && clamp.down == -89);
    // Downwards likewise, and a different pull restarts the count.
    CHECK(!clamp.observe(-70, -50));
    CHECK(!clamp.observe(-70, -40));
    CHECK(!clamp.observe(-70, -40));
    CHECK(clamp.observe(-70, -40));
    CHECK(clamp.down == -40);
    // Small pitches never teach a limit, and the camera seen past a limit gives it up.
    for (int i = 0; i < 10; ++i) CHECK(!clamp.observe(30, 10));
    clamp.observe(0, 75);
    CHECK(clamp.up == 75);
    // The turn stops at the limit; past it, the camera stays until the turn comes back.
    CHECK(limit_pitch_turn(50, 20, -60, 60) == 10);
    CHECK(limit_pitch_turn(50, 5, -60, 60) == 5);
    CHECK(limit_pitch_turn(-59, -5, -60, 60) == -1);
    CHECK(limit_pitch_turn(70, 5, -60, 60) == 0);
    CHECK(limit_pitch_turn(70, -5, -60, 60) == -5);
}

void test_unity_discovery() {
    using namespace gyro4all::unity;
    // Words, so that a short excluded word can't hit inside a name.
    CHECK((name_words("m_HorizontalAxis.Value") == std::vector<std::string>{"m", "horizontal", "axis", "value"}));
    CHECK((name_words("_cinemachineTargetPitch") == std::vector<std::string>{"cinemachine", "target", "pitch"}));
    CHECK((name_words("xRotation") == std::vector<std::string>{"x", "rotation"}));
    CHECK((name_words("rotX") == std::vector<std::string>{"rot", "x"}));
    // The names look states go by, and the settings and effects that aren't.
    CHECK(discovery_score("PlayerHeadRotation", "pitch", false) > 0);
    CHECK(discovery_score("Game.Foo", "_yaw", false) > 0);
    CHECK(discovery_score("Cinemachine.CinemachinePOV", "m_HorizontalAxis.Value", false) > 0);
    CHECK(discovery_score("Game.Mover", "m_Input", false) == 0);
    // Outer Wilds' pitch: a look word ("degrees") on a camera class, on the camera's rig, ahead of a bare "y" there.
    CHECK(discovery_score("PlayerCameraController", "_degreesY", true) > discovery_score("PlayerCameraController", "_offsetY", true));
    CHECK(discovery_score("PlayerCameraController", "_degreesY", true) >= 12);
    CHECK(discovery_score("PlayerCamera", "maxPitch", true) == 0);
    CHECK(discovery_score("PlayerCamera", "pitchSpeed", true) == 0);
    CHECK(discovery_score("PlayerCamera", "recoilPitch", true) == 0);
    CHECK(discovery_score("PlayerCamera", "lastYaw", true) == 0);
    // A bare x or y is nothing unless the script has to do with the camera.
    CHECK(discovery_score("Game.Enemy", "target.x", false) == 0);
    CHECK(discovery_score("PlayerLook", "look.x", false) > 0);
    CHECK(discovery_score("Game.Enemy", "health", false) == 0);
    CHECK(discovery_score("Game.Rig", "whatever", true) > 0);
    // On the camera's rig and with the right word first.
    CHECK(discovery_score("Game.Rig", "pitch", true) > discovery_score("Game.Rig", "pitch", false));
    CHECK(discovery_score("Game.Foo", "pitch", false) > discovery_score("Game.Foo", "rotationY", false));
    // A container's class name says nothing about the value inside it.
    CHECK(discovery_score("RoR2.CameraRigController", "_cameraMode.camToRawInstanceData[InstanceData].pitchYaw.pitch", true) > 8);
    CHECK(discovery_score("Game.Rig", "modes[SpeedData].pitch", false) > 0);
    CHECK(discovery_score("Game.Rig", "modes[Look].pitchSpeed", false) == 0);
    CHECK(camera_class_name("RoR2.CameraRigController") && camera_class_name("PlayerHeadRotation") && !camera_class_name("Game.Chair"));
    // A nudge's units: degrees, radians, reversed, and a value that turns both axes isn't a view axis.
    CHECK(std::abs(nudge_per_degree(0.25, 0.25, 0) - 1) < 1e-9);
    CHECK(std::abs(nudge_per_degree(0.25, -0.25, 0.01) + 1) < 1e-9);
    CHECK(std::abs(snap_per_degree(nudge_per_degree(0.25, 14.32, 0)) - 0.0174533) < 1e-4);
    CHECK(nudge_per_degree(0.25, 0.02, 0) == 0);
    CHECK(nudge_per_degree(0.25, 1, 0.8) == 0);
    // Viewfinder's PlayerMovement._lookInputVector: no angle does this with a nudge.
    CHECK(nudge_per_degree(0.25, 49.08, 0.32) == 0);
    CHECK(nudge_per_degree(0.25, 21.91, 0.26) == 0);
    // The body's Transform in the profile, with or without a pitch float.
    ProbeResult body;
    body.yaw_parent = 2;
    body.automatic = true;
    auto keys = profile_keys(body);
    CHECK(keys.size() == 2 && keys[0].first == "YawParent" && keys[0].second == "2" && keys[1].first == "Source");
    body.pitch_class = "Game.Look";
    body.pitch_path = "xRotation";
    body.pitch_per_degree = -1;
    keys = profile_keys(body);
    CHECK(keys[0].first == "ObjectType" && keys[1].first == "PitchPath");
    for (const auto& key : keys) CHECK(key.first != "YawPath" && key.first != "YawPerDegree");
    CHECK(std::none_of(keys.begin(), keys.end(), [](const auto& k) { return k.first == "PitchTransform"; }));
    body.pitch_transform = true;
    keys = profile_keys(body);
    CHECK(std::any_of(keys.begin(), keys.end(), [](const auto& k) { return k.first == "PitchTransform" && k.second == "1"; }));
    // The pitch's neighbours (Firewatch's camera mode offset), and a yaw found relative to the body.
    const auto siblings = sibling_components("cameraModeStack[vgCameraModeInstance].localOffset.y");
    CHECK(siblings.size() == 2 && siblings[0] == "cameraModeStack[vgCameraModeInstance].localOffset.x" &&
          siblings[1] == "cameraModeStack[vgCameraModeInstance].localOffset.z");
    CHECK(sibling_components("_pitch").empty() && sibling_components("look.w").empty() && sibling_components("a.yy").empty());
    ProbeResult relative_yaw;
    relative_yaw.pitch_class = relative_yaw.yaw_class = "vgCameraController";
    relative_yaw.pitch_path = "offset.y", relative_yaw.yaw_path = "offset.x";
    relative_yaw.pitch_per_degree = relative_yaw.yaw_per_degree = 1;
    keys = profile_keys(relative_yaw);
    CHECK(std::none_of(keys.begin(), keys.end(), [](const auto& k) { return k.first == "YawFromCamera"; }));
    relative_yaw.yaw_from_camera = true;
    keys = profile_keys(relative_yaw);
    CHECK(std::any_of(keys.begin(), keys.end(), [](const auto& k) { return k.first == "YawFromCamera" && k.second == "1"; }));
    // A search waits for a camera nobody is turning.
    ViewStillness pan;  // the title shot of Risk of Rain 2: under a degree a second
    bool settled = false;
    for (int ms = 0; ms <= 5000; ms += 100) settled = pan.add(1000 + ms, 127.5 + 0.85 * ms / 1000, -13.5, 1500) || settled;
    CHECK(!settled);
    ViewStillness sway;  // idle sway around one spot
    for (int ms = 0; ms < 1500; ms += 100) CHECK(!sway.add(1000 + ms, ms % 200 ? 0.2 : -0.2, 0.1, 1500));
    CHECK(sway.add(2500, 0.2, 0.1, 1500));
    ViewStillness seam;  // yaw crossing the +-180 seam isn't a turn
    CHECK(!seam.add(1000, 179.9, 0, 1500));
    CHECK(seam.add(2500, -179.9, 0, 1500));
    seam.reset();  // another camera: from the start
    CHECK(!seam.add(2600, -179.9, 0, 1500));
    // The first real turns judge a view found by itself.
    LookStateCheck check;
    CHECK(check.add(10, 10) == LookStateCheck::Verdict::None);
    CHECK(check.add(10, 10) == LookStateCheck::Verdict::Follows);
    CHECK(check.add(19, 0) == LookStateCheck::Verdict::None);  // decided: no more verdicts
    LookStateCheck ignored;
    CHECK(ignored.add(15, 0) == LookStateCheck::Verdict::None);
    CHECK(ignored.add(15, 0) == LookStateCheck::Verdict::None);
    CHECK(ignored.add(15, 0) == LookStateCheck::Verdict::None);
    CHECK(ignored.add(15, 0) == LookStateCheck::Verdict::Rejected);
    LookStateCheck cut;
    for (int i = 0; i < 4; ++i) CHECK(cut.add(90, 0) == LookStateCheck::Verdict::None);  // a shot cut isn't a turn
    // The profile's keys come back.
    const std::wstring path = temp_profile(L"unity_body");
    WritePrivateProfileStringW(L"Unity", L"ObjectType", L"Game.Look", path.c_str());
    WritePrivateProfileStringW(L"Unity", L"PitchPath", L"xRotation", path.c_str());
    CHECK(!load_unity_profile(path).enabled());
    WritePrivateProfileStringW(L"Unity", L"YawParent", L"1", path.c_str());
    WritePrivateProfileStringW(L"Unity", L"Source", L"auto", path.c_str());
    const auto loaded = load_unity_profile(path);
    CHECK(loaded.enabled() && loaded.yaw_parent == 1 && loaded.auto_found && loaded.has_view());
    const std::wstring alone = temp_profile(L"unity_body_alone");
    WritePrivateProfileStringW(L"Unity", L"YawParent", L"99", alone.c_str());
    CHECK(!load_unity_profile(alone).has_view());
    WritePrivateProfileStringW(L"Unity", L"YawParent", L"2", alone.c_str());
    CHECK(!load_unity_profile(alone).enabled() && load_unity_profile(alone).has_view());
}

void test_unity_probe_candidates() {
    using namespace gyro4all::unity;
    // A degree-valued float that runs with the yaw, against the two axes' samples.
    SlotStats yaw_value, pitch_down, unrelated, seldom;
    double yaw2 = 0, pitch2 = 0;
    const double yaw[] = {2, 3, -1, 4, 2, -3, 5, 1}, pitch[] = {1, -1, 2, 1, -1, 2, -1, -1};
    for (int i = 0; i < 8; ++i) {
        yaw2 += yaw[i] * yaw[i], pitch2 += pitch[i] * pitch[i];
        yaw_value.add(yaw[i], yaw[i], pitch[i]);
        pitch_down.add(-pitch[i], yaw[i], pitch[i]);
        unrelated.add(i % 2 ? 1.5 : -1.5, yaw[i], pitch[i]);
        seldom.add(i == 0 ? yaw[0] : 0, yaw[i], pitch[i]);
    }
    const AxisFit yaw_fit = fit_axis(yaw_value.value_yaw, yaw_value.value2, yaw2);
    CHECK(std::abs(yaw_fit.correlation - 1) < 1e-9 && std::abs(yaw_fit.per_degree - 1) < 1e-9);
    const AxisFit yaw_other = fit_axis(yaw_value.value_pitch, yaw_value.value2, pitch2);
    CHECK(candidate_score(yaw_fit, yaw_other, yaw_value.moved) > 1);
    // Pitch that counts downwards: the same, negative.
    const AxisFit pitch_fit = fit_axis(pitch_down.value_pitch, pitch_down.value2, pitch2);
    CHECK(std::abs(pitch_fit.correlation + 1) < 1e-9 && std::abs(pitch_fit.per_degree + 1) < 1e-9);
    CHECK(candidate_score(pitch_fit, fit_axis(pitch_down.value_yaw, pitch_down.value2, yaw2), pitch_down.moved) > 1);
    // What doesn't follow the axis, what follows both, or what moved too seldom is no candidate.
    CHECK(candidate_score(fit_axis(unrelated.value_yaw, unrelated.value2, yaw2),
                          fit_axis(unrelated.value_pitch, unrelated.value2, pitch2), unrelated.moved) == 0);
    CHECK(candidate_score({1, 1}, {0.9, 1}, 10) == 0);
    CHECK(candidate_score(fit_axis(seldom.value_yaw, seldom.value2, yaw2), {}, seldom.moved) == 0);
    CHECK(fit_axis(1, 0, 1).per_degree == 0 && fit_axis(1, 1, 0).per_degree == 0);
    // Real angles first: degrees over radians over a 0..1 axis, which still qualifies.
    const double degrees = candidate_score({0.95, 1}, {}, 10), radians = candidate_score({0.95, 0.0174}, {}, 10);
    const double axis01 = candidate_score({0.95, 0.008}, {}, 10);
    CHECK(degrees > radians && radians > axis01 && axis01 > 0);
    CHECK(snap_per_degree(-1.02) == -1 && snap_per_degree(0.5) == 0.5);
    CHECK(std::abs(snap_per_degree(0.0172) - 0.01745329) < 1e-6);
    // The live test: the camera turned as asked, and only on that axis.
    CHECK(turn_followed(3, 2.5, 0.2));
    CHECK(!turn_followed(3, 0, 0));
    CHECK(!turn_followed(3, 3, 3));
    CHECK(turn_followed(0, 0.5, -0.5));
    // What a verified result writes.
    ProbeResult r{"Game.Look", "pitch", "Game.Look", "state.yaw", "cam", -1.01, 0.0175};
    auto keys = profile_keys(r);
    const auto value = [&](const char* key) {
        for (const auto& k : keys)
            if (k.first == key) return k.second;
        return std::string("(none)");
    };
    CHECK(value("ObjectType") == "Game.Look" && value("YawObjectType") == "(none)" && value("CameraField") == "cam");
    CHECK(value("PitchPath") == "pitch" && value("YawPath") == "state.yaw");
    CHECK(value("PitchPerDegree") == "-1" && value("YawPerDegree") == "0.01745329");
    r.yaw_class = "Game.Body";
    r.camera_field.clear();
    keys = profile_keys(r);
    CHECK(value("YawObjectType") == "Game.Body" && value("CameraField") == "(none)");
    CHECK(value("PitchObject") == "(none)" && value("YawObject") == "(none)");
    // Transforms a script holds: no floats, no units.
    ProbeResult t;
    t.pitch_class = t.yaw_class = "Player";
    t.pitch_object = "cameraRigController";
    t.yaw_object = "characterController";
    keys = profile_keys(t);
    CHECK(value("ObjectType") == "Player" && value("YawObjectType") == "(none)" && value("PitchObject") == "cameraRigController" &&
          value("YawObject") == "characterController" && value("PitchPath") == "(none)" && value("PitchPerDegree") == "(none)" &&
          value("YawPath") == "(none)" && value("YawPerDegree") == "(none)");
    t.yaw_class = "Body";
    t.automatic = true;
    keys = profile_keys(t);
    CHECK(value("YawObjectType") == "Body" && value("Source") == "auto");
    // A pitch on an object with the body's yaw above the camera.
    t.yaw_object.clear();
    t.yaw_class.clear();
    t.yaw_parent = 2;
    keys = profile_keys(t);
    CHECK(value("YawParent") == "2" && value("YawObjectType") == "(none)" && value("YawObject") == "(none)");
    CHECK(value("ZoomReference") == "(none)" && value("ZoomReferenceAxis") == "(none)");
    // The unzoomed FOV found, with the axis the camera showed it in.
    t.zoom_reference = "cameraFovController._baseVerticalFOV";
    t.zoom_axis = gyro4all::unity::FovAxis::Horizontal;
    keys = profile_keys(t);
    CHECK(value("ZoomReference") == "cameraFovController._baseVerticalFOV" && value("ZoomReferenceAxis") == "Horizontal");
    t.pitch_class.clear();  // no ObjectType: nothing for the path to start from
    keys = profile_keys(t);
    CHECK(value("ZoomReference") == "(none)");
    // A Transform turns about one degree per degree, the same way; names put the body before a head for yaw.
    CHECK(transform_score({0.99, 1.0}, {}, 10) > 0);
    CHECK(transform_score({0.99, -1.0}, {}, 10) == 0 && transform_score({0.99, 0.0174}, {}, 10) == 0);
    CHECK(transform_score({0.99, 1.0}, {0.9, 1}, 10) == 0);
    CHECK(transform_discovery_score("Player", "characterController", false) >
          transform_discovery_score("Player", "cameraRigController", false));
    CHECK(transform_discovery_score("Player", "cameraRigController", true) > 0);
    CHECK(transform_discovery_score("Player", "characterController", true) == 0);
    CHECK(transform_discovery_score("Enemy", "target", false) == 0 && transform_discovery_score("Player", "weaponPivot", true) == 0);
    CHECK(object_rejection("Player", "cameraRigController") == "Player @cameraRigController");
}

// The game's own pitch float, found by matching the camera's pitch after the player's turns.
void test_unity_pitch_state() {
    using gyro4all::unity::PitchStateMatch;
    // 0: The Planet Crafter's _pitch (degrees, down); 1: a constant 0; 2: radians up; 3: an Euler angle kept in 0..360;
    // 4: a copy that lags a smoothed camera by one sample; 5: unreadable.
    PitchStateMatch match;
    match.reset(6);
    const double turns[] = {0, -12, -25, 8, 30, 15, -5, 40, 22, -30};
    double last = 0;
    for (double pitch : turns) {
        const float euler = float(pitch > 0 ? 360 - pitch : -pitch);
        match.add({float(-pitch), 0.0f, float(pitch * PitchStateMatch::kRadian), euler, float(-last), NAN}, pitch);
        last = pitch;
    }
    const auto found = match.found();
    auto has = [&](size_t index, double per_degree) {
        return std::any_of(found.begin(), found.end(),
                           [&](const auto& f) { return f.index == index && std::abs(f.per_degree - per_degree) < 1e-9; });
    };
    CHECK(has(0, -1));
    CHECK(has(2, PitchStateMatch::kRadian));
    CHECK(has(3, -1));
    CHECK(found.size() == 3);  // not the constant, the lagging copy or the unreadable one
    CHECK(!match.hopeless());
    // Too little of the range yet: nothing.
    PitchStateMatch small;
    small.reset(1);
    for (double pitch : {0.0, 1.0, 2.0, 3.0, 4.0, 5.0}) small.add({float(-pitch)}, pitch);
    CHECK(small.found().empty());
    // Nothing on the rig follows: hopeless, and an empty rig is too.
    PitchStateMatch none;
    none.reset(2);
    for (double pitch : turns) none.add({1.5f, float(pitch * 2)}, pitch);
    CHECK(none.found().empty() && none.hopeless());
    PitchStateMatch empty;
    empty.reset(0);
    CHECK(empty.hopeless());
}

int main() {
    test_unity_pitch_state();
    test_unity_profile();
    test_unity_aim_check();
    test_unity_pitch_clamp();
    test_unity_probe_candidates();
    test_unity_zoom_reference_axis();
    test_unity_discovery();
    test_unity_quaternions();
    test_unity_legacy_keys();
    test_unity_turn_preserves_roll_and_levels();
    test_rewritten_helper();
    test_match_single_member();
    test_match_base_follows();
    test_match_fast_path();
    test_match_slow_path();
    test_match_scene_round_trip();
    test_match_single_rewrite_doesnt_switch();
    test_match_lagging_camera_follows();
    test_match_needs_enough_gyro_and_time();
    test_match_look_input_resets();
    test_match_cutscene();
    test_match_write_failed();
    test_match_carry_limit();
    test_match_members_change();
    test_match_pitch_limit();
    test_match_all_ignored_keeps_followed();
    test_match_pitch_carry_limit();
    test_fov_conversion();
    test_load_missing();
    test_load_views();
    test_load_active_list_views();
    test_update_rotation_shape();
    test_add_view_new_file();
    test_add_view_yaw_only();
    test_add_view_keeps_profile();
    test_add_view_legacy_profile();
    test_add_view_duplicate();
    test_add_view_reserved_ids();
    test_load_trailing_tab();
    test_add_view_property();
    test_add_view_rejects_empty();
    test_split();
    test_stick_model();
    test_stick_fallback_not_when_writes_last();
    test_stick_fallback_not_at_pitch_limit();
    test_stick_fallback_starts_the_right_way();
    test_stick_fallback_ends_when_writes_last_again();
    test_stick_fallback_ends_on_another_camera();
    test_stick_fallback_gives_up_without_effect();
    test_stick_fallback_leaves_player_stick_alone();
    test_stick_fallback_forced();
    test_stick_fallback_blind();
    test_stick_fallback_learns_linear_axial();
    test_stick_fallback_learns_curved_radial_late();
    test_stick_fallback_learns_fast_axial();
    test_stick_fallback_learns_with_noise_and_recoil();
    test_stick_fallback_edge_acceleration();
    test_stick_fallback_high_frame_rate();
    test_stick_fallback_learns_inverted_pitch();
    test_stick_fallback_rejects_implausible();
    test_stick_fallback_learns_lag();
    test_stick_fallback_pitch_limit();
    test_stick_fallback_slow_aim();
    test_stick_fallback_restored_model();
    test_stick_fallback_default_model_any_game();
    test_stick_camera_deadzone();
    test_stick_camera_full_speed();
    test_stick_camera_diagonal();
    test_stick_camera_curve();
    test_stick_camera_edge_ramp();
    test_stick_camera_dt_cap();
    test_stick_camera_invert_y();
    test_pacer_steady_turn_even();
    test_pacer_variable_frames();
    test_pacer_stop_keeps_total_no_step_back();
    test_pacer_reverse();
    test_pacer_burst_passthrough_and_off();
    test_pacer_stall_at_once();
    test_pacer_fixed_delay();
    test_zoom_scale();
    test_zoom_strength();
    test_zoom_levels_basic();
    test_zoom_levels_remember();
    test_zoom_ramp();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

