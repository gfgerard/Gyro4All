// Unit tests for the parts of Gyro4All that need no game: the Unreal module's game profile (ue_profile.cpp), the
// core's Match=Camera choice (camera_match.hpp), its stick fallback (stick_fallback.cpp, stick_response.cpp) and stick camera (stick_camera.cpp). Built with Gyro4All (target gyro4all_tests); run with
// tools/test-gyro4all.sh.
#include "camera_match.hpp"
#include "stick_camera.hpp"
#include "stick_fallback.hpp"
#include "ue_profile.hpp"
#include "ue_update_rotation_match.hpp"
#include "util.hpp"

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
               "[View.Default]\r\nLabel=On foot\r\n\r\n"
               "[View.InCar]\r\nId=2\r\nViewTargetClass=BP_Car_C\r\nProperty=CamRot\r\n\r\n"
               "[View.Clamp]\r\nId=3\r\nMatch=camera\r\nObject=Camera.@SBZClampCameraRotationModifier\r\nOffsets=0x68\r\n"
               "RotatorSize=24\r\n\r\n"
               "[View.Bad]\r\nId=4\r\nMatch=Camera\r\n\r\n"
               "[View.Dup]\r\nId=3\r\nMatch=Camera\r\nProperty=X\r\n");
    const std::vector<View> views = load_views(path);
    CHECK(views.size() == 3);
    CHECK(views[0].label == "On foot");
    const View* car = by_id(views, 2);
    CHECK(car && car->view_target_class == "BP_Car_C" && !car->match_camera && car->property == "CamRot");
    const View* clamp = by_id(views, 3);
    CHECK(clamp && clamp->match_camera && clamp->view_target_class.empty());
    CHECK(clamp && clamp->object.size() == 2 && clamp->object[1] == "@SBZClampCameraRotationModifier");
    CHECK(clamp && clamp->offsets.size() == 1 && clamp->offsets[0] == 0x68 && clamp->rotator_size == 24);
    CHECK(clamp && clamp->label == "Clamp" && clamp->parent == 1);
    CHECK(has_views(path));
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

int main() {
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
    test_load_missing();
    test_load_views();
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
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

