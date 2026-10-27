#include "ue_probe.hpp"

#include "engine.hpp"
#include "host.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "settings.hpp"
#include "ue.hpp"
#include "ue_engine.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

// The camera probe (Probe=1): finds the rotation that turns the camera of a game whose camera ignores
// ControlRotation, tests it live and writes the game's profile. See ADVANCED.md, "Finding the camera of a new game".
namespace gyro4all::unreal {
namespace {

// Diagnostics: [Gyro4All] Probe=1 captures the camera state automatically on stick or mouse turns (see auto_probe)
// and enables F7 (snapshot) / F8 (log what changed since the snapshot) for manual captures.
bool g_probe = false, g_probe_snap_down = false, g_probe_diff_down = false;
// The game had a profile when it started; the probe started with Probe=1 writes the profile itself only if it didn't
// (and then keeps updating the one it created this session), else it adds its camera as a view (see finish_tests).
bool g_profile_existed = false, g_profile_created = false;
// The probe can also start during play, without Probe=1 or a restart: from ProbeKey, or by itself (AutoProbe=1) when the
// camera stops following the gyro. Its verified camera is then added to the profile as a Match=Camera view.
bool g_probe_runtime = false;       // the running probe was started during play
bool g_probe_automatic = false;     // ... by AutoProbe (it gives up after a while without captures)
bool g_auto_probe_setting = true;   // AutoProbe=
int g_auto_probe_starts = 0;        // automatic starts this session (limited)
ULONGLONG g_probe_started_at = 0, g_next_auto_probe = 0;
struct Key {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
    std::string name;
};
Key g_probe_key;  // ProbeKey=
bool g_probe_key_down = false;

const char kProbeSettings[] =
    "; 1 = find the camera of a game whose camera ignores the gyro: in gameplay, turn the camera with the\r\n"
    ";     RIGHT STICK or MOUSE (more than 30 degrees) and stop, 3-4 times (the gyro is paused meanwhile). Gyro4All then\r\n"
    ";     writes the game's profile (Gyro4All\\profiles\\<game exe>.ini; to one that exists, it adds the camera as a view).\r\n"
    ";     Once a camera is verified, Gyro4All sets this back to 0 by itself. Results: Gyro4All\\Gyro4All-probe.log.\r\n"
    "Probe=0\r\n"
    "; Learning a camera during the game, without restarting (results also in Gyro4All\\Gyro4All-probe.log):\r\n"
    ";   ProbeKey: starts the probe right where you are, and cancels it. F1..F24, optionally with Ctrl+, Shift+ or Alt+;\r\n"
    ";     empty = no key.\r\n"
    ";   AutoProbe=1: when the camera stops following the gyro while you turn it yourself, the probe starts by itself.\r\n"
    ";   A camera found this way is added to the game's profile as a new view and used at once. 0 = off.\r\n"
    "ProbeKey=Ctrl+F7\r\n"
    "AutoProbe=1\r\n";

std::string probe_header() {
    return "Gyro4All " GYRO4ALL_VERSION " camera probe (recreated every game session)\n"
           "\n"
           "How to use it (automatic):\n"
           "  1. In gameplay, stand still for a second. The gyro is paused while the probe captures.\n"
           "  2. Turn the camera with the RIGHT STICK or the MOUSE: more than 30 degrees sideways (and a bit up or\n"
           "     down), then stop. Gyro4All captures the turn by itself (turns without them, e.g. cutscenes, are ignored).\n"
           "  3. Repeat 3 or 4 times. From the second capture on, the game's profile is written to\n"
           "     Gyro4All\\profiles\\<game>.ini (and copied at the end of this file). Once a camera is verified live,\n"
           "     Gyro4All sets Probe=0 in Gyro4All\\Gyro4All.ini by itself: restart the game and the gyro turns the camera.\n"
           "     If the game already had a profile, or the probe was started during play (ProbeKey, or AutoProbe when\n"
           "     a camera stops following the gyro), the verified camera is added to the profile as a new view\n"
           "     (Match=Camera) and used at once, without a restart.\n"
           "  Manual alternative: F7 = snapshot, turn with the stick, F8 = diff (pressing F7/F8 turns the automatic\n"
           "  capture off for the session).\n"
           "\n"
           "How to read it (ADVANCED.md, \"Finding the camera of a new game\"):\n"
           "  rot <path>.<Property>         a reflected rotator that changed  -> profile: Object=<path>, Property=<Property>\n"
           "  raw <path>+0xNN double/float  non-reflected memory that follows the camera -> Object=<path>, Offsets=0xNN\n"
           "  Ignore Camera(...) and transform components: they are the camera's output, not its input (except a\n"
           "  component's RelativeRotation that the suggested profile marks WorldRotation=1).\n"
           "  The path is the part before \"(\"; the class in parentheses is only informative.\n"
           "\n";
}

void take_snapshot(const Scene& s) {
    probe_log("probe: roots pc=%s pawn=%s camera=%s target=%s", ue::name_of(s.pc).c_str(), ue::name_of(s.pawn).c_str(),
              ue::name_of(s.camera).c_str(), ue::name_of(s.view_target).c_str());
    ue::probe_snapshot({{"PC", s.pc}, {"Pawn", s.pawn}, {"Camera", s.camera}, {"ViewTarget", s.view_target}});
}

// Writes the probe's result: the game's profile itself only when the live test verified it and the game had no
// profile when it started (or the probe created it this session); otherwise <exe>.suggested.ini next to it. An
// unverified guess never becomes the profile. Returns where it went (Nothing = no result yet).
enum class Written { Nothing, Profile, Suggested };

// The probe's verified camera becomes a new Match=Camera view in the existing (or new) profile, rather than the
// profile's default view: when it was started during play (that camera is an extra one, the default view works
// elsewhere), or when the game already has a profile (never overwritten).
bool probe_adds_view() { return g_probe_runtime || g_profile_existed; }

Written write_suggestion(bool& usable, bool& verified) {
    usable = verified = false;
    std::string summary;
    const std::string probe = ue::probe_suggestion(narrow(settings::exe_stem()), summary, false, verified);
    if (probe.empty()) return Written::Nothing;
    usable = probe.find("[View.Default]") != std::string::npos;
    // A probe started during play, or in a game that already has a profile, adds its camera as a view instead.
    const bool as_profile = usable && verified && !probe_adds_view() && (!g_profile_existed || g_profile_created);
    const std::string text = as_profile ? ue::probe_suggestion(narrow(settings::exe_stem()), summary, true, verified) : probe;
    const std::wstring path = settings::mod_dir() + L"\\profiles\\" + settings::exe_stem() + (as_profile ? L".ini" : L".suggested.ini");
    // The profile's [Gyro4All] settings (e.g. DualSenseAsDualShock4, learned earlier) survive the probe rewriting it, and
    // so does a [GyroUE] section of a profile from GyroUE.
    std::wstring kept[2];
    const wchar_t* sections[2] = {settings::kSection, settings::kLegacySection};
    for (int i = 0; i < 2; ++i) {
        kept[i].assign(4096, L'\0');
        kept[i].resize(as_profile ? GetPrivateProfileSectionW(sections[i], kept[i].data(), DWORD(kept[i].size()), path.c_str()) : 0);
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0) file = nullptr;
    const bool written = file && std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (file) std::fclose(file);
    for (int i = 0; i < 2; ++i)
        if (written && !kept[i].empty()) WritePrivateProfileSectionW(sections[i], kept[i].c_str(), path.c_str());
    if (written && as_profile) g_profile_created = true;
    const char* what = as_profile ? "game profile" : "suggested profile";
    probe_log("probe: %s (%s)%s:\n%s", what, summary.c_str(),
              written ? (" written to " + narrow(path)).c_str() : " (could not write the file)", text.c_str());
    log("probe: %s: %s -> %s", what, summary.c_str(), written ? narrow(path).c_str() : "write failed, see Gyro4All-probe.log");
    if (!written) return Written::Nothing;
    return as_profile ? Written::Profile : Written::Suggested;
}

// On-screen result of a manual diff (F8): no live test there, so only an unverified suggestion is written.
void notify_manual_diff() {
    bool usable = false, verified = false;
    const Written written = write_suggestion(usable, verified);
    std::wstring text = L"Gyro4All probe: diff done.\n";
    if (written == Written::Nothing)
        text += L"Turn the camera with the right stick, then F7/F8 again (two diffs are needed).";
    else if (usable)
        text += L"Unverified guess written to Gyro4All\\profiles\\" + settings::exe_stem() +
                L".suggested.ini\nFor a verified profile, use the automatic probe (Probe=1 without F7/F8).";
    else
        text += L"No camera rotation found yet (see Gyro4All-probe.log).";
    notify::show(text, written == Written::Nothing ? 10 : 20);
}

// Automatic probe (Probe=1), guided by on-screen notices:
// 1. Captures: a snapshot once the camera rests, a scored diff when the player has turned it more than 30 degrees
//    with the right stick and stopped again. Every rotation that followed the camera becomes a candidate.
// 2. Live test, after kCapturesForTest captures, while the player doesn't touch the controller: each candidate
//    gets small turns written into it, exactly as a profile would write them. It passes only if the camera turns by
//    that amount and comes back when the turn is undone, and again in a second round. An output (the game rewrites
//    it every frame) or a copy the camera doesn't read fails, which is what the captures alone can't tell apart.
// 3. A verified rotation becomes the game's profile (or <exe>.suggested.ini). Without one: more captures, then the
//    new candidates are tested; after kMaxAutoCaptures only an unverified guess is written, never as the profile.
// Off as soon as F7/F8 are used.
struct AutoProbe {
    bool enabled = true, armed = false, announced = false, have_last = false;
    bool done = false;            // finished: verified, or out of captures
    bool stick_turn = false;      // the right stick was deflected since the snapshot
    bool stick_data = false;      // GyroLib has reported the right stick at least once (else no stick filter)
    double snap_pitch = 0, snap_yaw = 0, last_pitch = 0, last_yaw = 0;
    ue::Obj pawn = 0;
    ue::Obj target_class = 0;     // view target class of the captures: the live test runs on the same camera
    ULONGLONG still_since = 0, snapshot_at = 0, next_snapshot = 0;
    int captures = 0;
    // How far the camera moves when the stick turns it: a third-person camera orbits around the character (its
    // location moves on a circle of this radius), a first-person one turns on the spot (radius about 0). Measured
    // in every capture; the live test checks that a candidate's turn moves the camera the same way.
    bool snap_loc_ok = false;
    double snap_loc[3] = {};
    std::vector<double> radii;
} g_auto;
constexpr int kMaxAutoCaptures = 8, kCapturesForTest = 3, kMaxTests = 40;
constexpr double kTestTurn = 3.0, kTestTolerance = 0.5;
constexpr char kPitchFollowed[] = "pitch followed";  // in a live test note: pitch writes moved the camera too
constexpr double kOrbitMin = 30;  // world units: below this the camera turns on the spot (first person), no orbit check
constexpr double kDegToRad = 3.14159265358979323846 / 180;

// Angle in degrees between the view directions of two rotations.
double view_angle(double p0, double y0, double p1, double y1) {
    const double r = kDegToRad;
    const double a[3] = {std::cos(p0 * r) * std::cos(y0 * r), std::cos(p0 * r) * std::sin(y0 * r), std::sin(p0 * r)};
    const double b[3] = {std::cos(p1 * r) * std::cos(y1 * r), std::cos(p1 * r) * std::sin(y1 * r), std::sin(p1 * r)};
    return std::acos(std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0)) / r;
}

double distance(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// The camera's orbit radius from the captures (median), 0 if unknown.
double orbit_radius() {
    if (g_auto.radii.empty()) return 0;
    std::vector<double> r = g_auto.radii;
    std::sort(r.begin(), r.end());
    return r[r.size() / 2];
}



struct LiveTest {
    enum Phase { Off, Settle, Null, YawOut, YawBack, PitchOut, PitchBack } phase = Off;
    std::vector<ue::ProbeGroup> queue;
    size_t index = 0;
    int round = 1, attempts = 0;
    std::vector<std::string> passed;  // ids that moved the camera in round 1 (tested again in round 2)
    View view;                        // the candidate, as a profile view
    double base_p = 0, base_y = 0;    // camera before the current turn
    double val_y = 0, pawn_yaw = 0, delta_p = 0;
    double written_p = 0, written_y = 0;  // turn currently written into the candidate (undone on abort)
    bool base_loc_ok = false;
    double base_loc[3] = {};
    std::string orbit_note;
    bool yaw_ok = false, pitch_ok = false, turns_pawn = false;
    ULONGLONG phase_at = 0, ok_since = 0, last_pause_notice = 0;
} g_test;

View test_view(const ue::ProbeGroup& g) {
    return test_view_for(g.object, g.property, g.offsets, g.size, g.world);
}

std::string group_text(const ue::ProbeGroup& g) {
    std::string s = g.object;
    if (!g.property.empty()) return s + "." + g.property;
    s += " raw";
    for (size_t i = 0; i < g.offsets.size(); ++i) s += std::format("{}0x{:X}", i ? "," : " ", g.offsets[i]);
    return s;
}

void finish_tests();

void next_candidate() {
    ++g_test.index;
    g_test.attempts = 0;
    g_test.phase = LiveTest::Settle;  // a turn still written (undo failed) is undone there, before the next test
    if (g_test.index < g_test.queue.size()) {
        notify::show(std::format(L"Gyro4All probe: testing rotation {} of {}{}.\nDON'T TOUCH THE CONTROLLER (the camera "
                                 L"twitches a little).",
                                 g_test.index + 1, g_test.queue.size(), g_test.round == 2 ? L" (second round)" : L""),
                     8);
        return;
    }
    if (g_test.round == 1 && !g_test.passed.empty()) {  // round 2: the ones that moved the camera, once more
        std::vector<ue::ProbeGroup> again;
        for (const ue::ProbeGroup& g : ue::probe_groups())
            if (std::find(g_test.passed.begin(), g_test.passed.end(), g.id) != g_test.passed.end()) again.push_back(g);
        g_test.queue = std::move(again);
        g_test.index = 0;
        g_test.round = 2;
        probe_log("probe test: second round, %zu rotation(s)", g_test.queue.size());
        if (!g_test.queue.empty()) {
            notify::show(std::format(L"Gyro4All probe: second round, rotation 1 of {}.\nDON'T TOUCH THE CONTROLLER.",
                                     g_test.queue.size()),
                         8);
            return;
        }
    }
    finish_tests();
}

void test_result(int verdict, const std::string& note) {
    const ue::ProbeGroup& g = g_test.queue[g_test.index];
    const char* what = verdict == ue::kMovesCamera ? "MOVES THE CAMERA" : verdict == ue::kOverwritten ? "overwritten (output)"
                     : verdict == ue::kNoEffect    ? "kept, camera didn't move"
                     : verdict == ue::kInconsistent ? "inconsistent"
                     : verdict == ue::kTurnsInPlace ? "turned the view in place (no orbit)" : "not testable";
    probe_log("probe test (round %d): %s: %s%s%s", g_test.round, group_text(g).c_str(), what, note.empty() ? "" : ": ",
              note.c_str());
    log("probe test (round %d): %s: %s%s%s", g_test.round, group_text(g).c_str(), what, note.empty() ? "" : ": ", note.c_str());
    if (verdict == ue::kMovesCamera && g_test.round == 1) g_test.passed.push_back(g.id);  // verdict after round 2
    else if (verdict != ue::kMovesCamera && g_test.round == 2) ue::probe_verdict(g.id, ue::kInconsistent, note);
    else ue::probe_verdict(g.id, verdict, note);
    next_candidate();
}

// Undo whatever turn is written into the candidate. If it can't be written now (left gameplay), it stays pending
// and Settle retries it before anything else.
bool undo_turn(const Scene& s) {
    if (g_test.written_p == 0 && g_test.written_y == 0) return true;
    if (!apply(g_test.view, s, -g_test.written_p, -g_test.written_y)) return false;
    g_test.written_p = g_test.written_y = 0;
    return true;
}

void abort_test(const Scene& s, const char* why) {
    undo_turn(s);
    g_test.phase = LiveTest::Settle;
    if (++g_test.attempts > 3) {
        test_result(ue::kUntestable, std::string("interrupted 3 times (") + why + ")");
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_test.last_pause_notice > 4000) {
        g_test.last_pause_notice = now;
        notify::show(L"Gyro4All probe: test paused. Stay in game and DON'T TOUCH THE CONTROLLER.", 5);
    }
}

bool start_tests() {
    std::vector<ue::ProbeGroup> queue;
    for (const ue::ProbeGroup& g : ue::probe_groups())
        if (g.verdict == ue::kUntested && queue.size() < size_t(kMaxTests)) queue.push_back(g);
    if (queue.empty() || g_test.written_p != 0 || g_test.written_y != 0) return false;
    const ULONGLONG keep_notice = g_test.last_pause_notice;
    g_test = {};
    g_test.last_pause_notice = keep_notice;
    g_test.queue = std::move(queue);
    g_test.phase = LiveTest::Settle;
    g_auto.armed = false;
    probe_log("probe test: %zu rotation(s) to test after %d captures", g_test.queue.size(), g_auto.captures);
    log("probe: live test of %zu rotation(s) that followed the camera (the player must not touch the controller)",
        g_test.queue.size());
    notify::show(std::format(L"Gyro4All probe: {} captures done. Now testing {} rotation(s) live.\n"
                             L"Stay in game and DON'T TOUCH THE CONTROLLER: the camera will twitch a little.",
                             g_auto.captures, g_test.queue.size()),
                 10);
    return true;
}

// After a verified camera the probe's job is done: Probe=0 in Gyro4All.ini (and in the game's profile, if it sets
// Probe itself), so the next start doesn't keep the gyro paused for another probe run.
bool disable_probe() {
    // Written to [Gyro4All], which wins over a [GyroUE] Probe=1 left from GyroUE.
    bool ok = WritePrivateProfileStringW(settings::kSection, L"Probe", L"0", settings::settings_path().c_str()) != 0;
    std::wstring value;
    if (settings::read(settings::profile_path(), L"Probe", value))
        ok = WritePrivateProfileStringW(settings::kSection, L"Probe", L"0", settings::profile_path().c_str()) != 0 && ok;
    log("probe: %s", ok ? "Probe=0 written (the probe won't run on the next start)" : "couldn't write Probe=0, set it by hand");
    return ok;
}

// The probe is over for this session (until the probe key or AutoProbe starts it again).
void probe_end(const char* why) {
    if (!g_probe) return;
    g_probe = false;
    g_test.phase = LiveTest::Off;
    g_probe_runtime = g_probe_automatic = false;
    g_next_auto_probe = GetTickCount64() + 30000;
    log("probe: stopped (%s)", why);
    probe_log("probe: stopped (%s)", why);
}

// A probe started during play (or in a game with a profile) verified a camera: it becomes a Match=Camera view of the
// profile, used at once (the engine reads the file again). The profile's other views and settings are kept.
void learn_camera(const ue::ProbeGroup& g, bool yaw_only) {
    LearnedView learned;
    learned.object = g.object;
    learned.property = g.property;
    learned.offsets = g.offsets;
    learned.rotator_size = g.size ? g.size : 24;
    learned.world_rotation = g.world;
    learned.pitch_view = yaw_only ? 1 : 0;
    learned.label = g.cls.empty() ? "Learned camera" : g.cls;
    SYSTEMTIME t;
    GetLocalTime(&t);
    learned.note = std::format("Learned by the Gyro4All {} camera probe on {:04}-{:02}-{:02} {:02}:{:02} (view target {}).\n"
                               "Live test: {}. Used while the camera follows it (Match=Camera).{}",
                               GYRO4ALL_VERSION, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                               ue::name_of(g_auto.target_class), g.note.empty() ? "moved the camera" : g.note,
                               yaw_only ? "\nYaw only: its pitch goes to the default view (PitchView=1)." : "");
    const std::vector<uint32_t> reserved(std::begin(kSensitivityViewIds), std::end(kSensitivityViewIds));
    uint32_t id = 0;
    std::string key;
    CreateDirectoryW((settings::mod_dir() + L"\\profiles").c_str(), nullptr);
    const AddResult r = add_view(settings::profile_path(), narrow(settings::exe_stem()), learned, reserved, id, key);
    const std::string rotation =
        g.object + (g.property.empty() ? std::format(" raw x{}", g.offsets.size()) : "." + g.property);
    if (r == AddResult::Added) {
        log("probe: camera learned: %s added to %s as view %u '%s' (Match=Camera)", rotation.c_str(),
            narrow(settings::profile_path()).c_str(), id, learned.label.c_str());
        probe_log("probe: camera learned: %s added to the profile as [View.%s] (Id=%u, Match=Camera)", rotation.c_str(),
                  key.c_str(), id);
        notify::show(L"Gyro4All: camera found and VERIFIED, added to this game's profile.\nThe gyro turns it from now on "
                     L"(no restart needed); the other cameras keep working.", 20);
        profile_changed();
    } else if (r == AddResult::Duplicate) {
        log("probe: camera learned: %s is already view %u of the profile", rotation.c_str(), id);
        notify::show(L"Gyro4All: camera found and VERIFIED; the profile already has it.\nThe gyro switches to it when "
                     L"the camera follows it.", 15);
    } else {
        log("probe: camera learned (%s) but %s couldn't be written", rotation.c_str(), narrow(settings::profile_path()).c_str());
        notify::show(L"Gyro4All: camera found, but the game's profile couldn't be written.\nSee Gyro4All\\Gyro4All.log.", 15);
    }
    g_auto.done = true;
    if (!g_probe_runtime) disable_probe();
    probe_end("camera learned");
}

void finish_tests() {
    g_test.phase = LiveTest::Off;
    if (probe_adds_view()) {
        const std::vector<ue::ProbeGroup> groups = ue::probe_groups();
        // A rotation that turns yaw and pitch first. A yaw-only one (Dead Island 2: the pawn's capsule, which the
        // first-person camera follows in yaw while pitch stays in ControlRotation) is learned with PitchView=1: its
        // pitch goes to the default view, else the view would lose pitch.
        auto pitch_ok = [](const ue::ProbeGroup& g) { return g.note.find(kPitchFollowed) != std::string::npos; };
        auto verified = std::find_if(groups.begin(), groups.end(), [&](const ue::ProbeGroup& g) {
            return g.verdict == ue::kMovesCamera && pitch_ok(g);
        });
        if (verified == groups.end())
            verified = std::find_if(groups.begin(), groups.end(),
                                    [](const ue::ProbeGroup& g) { return g.verdict == ue::kMovesCamera; });
        if (verified != groups.end()) {
            log("probe: done, a rotation moved the camera in both rounds of the live test");
            return learn_camera(*verified, !pitch_ok(*verified));
        }
    }
    bool usable = false, verified = false;
    const Written written = write_suggestion(usable, verified);
    const std::wstring file = L"Gyro4All\\profiles\\" + settings::exe_stem() + (written == Written::Profile ? L".ini" : L".suggested.ini");
    if (verified) {
        g_auto.done = true;
        log("probe: done, a rotation moved the camera in both rounds of the live test");
        const bool off = written != Written::Nothing && disable_probe();
        const std::wstring probe_off = off ? L"The probe is now off (Probe=0)." : L"Then set Probe=0 in Gyro4All\\Gyro4All.ini.";
        notify::show(written == Written::Profile
                         ? L"Gyro4All: camera found and VERIFIED, profile created.\nRESTART THE GAME: the gyro will then "
                           L"turn the camera.\n" + probe_off
                         : L"Gyro4All: camera found and VERIFIED.\nThis game already has a profile, so it went to " + file +
                               L"\nRename it to " + settings::exe_stem() + L".ini (replacing the old one), then RESTART THE GAME: "
                               L"the gyro will then turn the camera.\n" + probe_off,
                     30);
        return;
    }
    if (g_auto.captures >= kMaxAutoCaptures) {
        g_auto.done = true;
        log("probe: done, no rotation moved the camera in the live test");
        notify::show(L"Gyro4All probe: no rotation moved the camera, so no profile was created.\n" +
                         (usable ? L"An unverified guess is in " + file + L" (it will probably not work).\n" : std::wstring()) +
                         L"See Gyro4All\\Gyro4All-probe.log. The gyro works again (default camera).",
                     30);
        if (g_probe_runtime) probe_end("no rotation moved the camera");
        return;
    }
    notify::show(L"Gyro4All probe: none of these rotations moved the camera.\nTurn the camera again with the RIGHT "
                 L"STICK or the MOUSE (more than 30°), then stop, to find more.",
                 15);
}

// One frame of the live test. valid: in game, unpaused, no cursor, focused (the same rules as the captures).
void run_live_test(const Scene& s, bool valid, ULONGLONG still_for) {
    const ULONGLONG now = GetTickCount64();
    const bool writing = g_test.phase != LiveTest::Settle && g_test.phase != LiveTest::Null;
    if (!valid || !s.pov_ok || ue::class_of(s.view_target) != g_auto.target_class || look_input()) {
        if (writing || g_test.phase == LiveTest::Null)
            abort_test(s, !valid ? "left gameplay" : !s.pov_ok ? "no camera" : ue::class_of(s.view_target) != g_auto.target_class
                                                                                  ? "another camera" : "stick or mouse moved");
        return;
    }
    const double dy = wrap180(s.pov_yaw - g_test.base_y), dp = wrap180(s.pov_pitch - g_test.base_p);
    // A condition held for `ms` (the camera may ease into a written turn).
    auto held = [&](bool on, ULONGLONG ms) {
        if (!on) return g_test.ok_since = 0, false;
        if (!g_test.ok_since) g_test.ok_since = now;
        return now - g_test.ok_since >= ms;
    };
    auto enter = [&](LiveTest::Phase p) {
        g_test.phase = p;
        g_test.phase_at = now;
        g_test.ok_since = 0;
    };
    const ULONGLONG in_phase = now - g_test.phase_at;
    switch (g_test.phase) {
    case LiveTest::Off:
        return;
    case LiveTest::Settle:  // the camera rests
        if (!undo_turn(s)) return;
        if (still_for < 600) return;
        g_test.base_p = s.pov_pitch, g_test.base_y = s.pov_yaw;
        enter(LiveTest::Null);
        return;
    case LiveTest::Null: {  // and keeps resting without our writes (no drift, no idle animation turning it)
        if (std::abs(dy) > 0.2 || std::abs(dp) > 0.2) {
            enter(LiveTest::Settle);
            return;
        }
        if (in_phase < 250) return;
        const ue::ProbeGroup& g = g_test.queue[g_test.index];
        g_test.view = test_view(g);
        double p0 = 0;
        if (!read_view_rotation(g_test.view, s, p0, g_test.val_y))
            return test_result(ue::kUntestable, "its object path doesn't resolve now");
        g_test.pawn_yaw = pawn_yaw(s);
        g_test.yaw_ok = g_test.pitch_ok = g_test.turns_pawn = false;
        g_test.base_loc_ok = s.loc_ok;
        if (s.loc_ok) std::copy(s.pov_loc, s.pov_loc + 3, g_test.base_loc);
        if (!apply(g_test.view, s, 0, kTestTurn)) return test_result(ue::kUntestable, "couldn't be written");
        g_test.written_y = kTestTurn;
        enter(LiveTest::YawOut);
        return;
    }
    case LiveTest::YawOut: {  // the camera turns by the written yaw, and stays there
        const bool on = std::abs(dy - kTestTurn) < kTestTolerance && std::abs(dp) < kTestTolerance;
        if (held(on, 150)) g_test.yaw_ok = true;
        else if (in_phase < 700) return;
        double p = 0, y = 0;
        const bool kept = read_view_rotation(g_test.view, s, p, y) && std::abs(wrap180(y - g_test.val_y - kTestTurn)) < kTestTolerance;
        const double pawn_now = pawn_yaw(s);
        g_test.turns_pawn = std::isfinite(pawn_now) && std::isfinite(g_test.pawn_yaw) &&
                            std::abs(wrap180(pawn_now - g_test.pawn_yaw - kTestTurn)) < kTestTolerance;
        // Third-person camera: the stick orbits it around the character, so its location moves; a candidate that
        // only turns the view on the spot (a free camera) is not the look input.
        const double radius = orbit_radius();
        g_test.orbit_note.clear();
        if (g_test.yaw_ok && radius >= kOrbitMin && g_test.base_loc_ok && s.loc_ok) {
            const double angle = view_angle(g_test.base_p, g_test.base_y, s.pov_pitch, s.pov_yaw);
            const double expected = 2 * radius * std::sin(angle * kDegToRad / 2);
            const double moved = distance(g_test.base_loc, s.pov_loc);
            g_test.orbit_note = std::format("camera moved {:.1f} for an expected {:.1f} (orbit radius {:.0f})", moved,
                                            expected, radius);
            if (moved < expected * 0.4) {
                undo_turn(s);
                return test_result(ue::kTurnsInPlace, g_test.orbit_note);
            }
        }
        undo_turn(s);
        if (!g_test.yaw_ok)
            return test_result(kept ? ue::kNoEffect : ue::kOverwritten,
                               kept ? "the value kept the turn, the camera didn't follow" : "the game rewrote the value");
        enter(LiveTest::YawBack);
        return;
    }
    case LiveTest::YawBack:  // undoing it brings the camera back
        if (held(std::abs(dy) < kTestTolerance && std::abs(dp) < kTestTolerance, 100)) {
            g_test.base_p = s.pov_pitch, g_test.base_y = s.pov_yaw;
            g_test.delta_p = g_test.base_p > 0 ? -kTestTurn : kTestTurn;  // towards level: away from the pitch limits
            if (!apply(g_test.view, s, g_test.delta_p, 0)) return test_result(ue::kUntestable, "pitch couldn't be written");
            g_test.written_p = g_test.delta_p;
            enter(LiveTest::PitchOut);
            return;
        }
        if (in_phase >= 700) return test_result(ue::kInconsistent, "the camera turned but didn't come back when undone");
        return;
    case LiveTest::PitchOut: {
        const bool on = std::abs(dp - g_test.delta_p) < kTestTolerance && std::abs(dy) < kTestTolerance;
        if (held(on, 150)) g_test.pitch_ok = true;
        else if (in_phase < 700) return;
        undo_turn(s);
        enter(LiveTest::PitchBack);
        return;
    }
    case LiveTest::PitchBack:
        if (!held(std::abs(dy) < kTestTolerance && std::abs(dp) < kTestTolerance, 100) && in_phase < 700) return;
        test_result(ue::kMovesCamera, std::string("yaw followed") +
                                          (g_test.pitch_ok ? std::string(", ") + kPitchFollowed : ", pitch didn't move (may be kept elsewhere)") +
                                          (g_test.turns_pawn ? ", it also turns the character" : "") +
                                          (g_test.orbit_note.empty() ? "" : ", " + g_test.orbit_note));
        return;
    }
}

void automatic(const Scene& s) {
    if (!g_auto.enabled || g_auto.done) return;
    const ULONGLONG now = GetTickCount64();
    const bool valid = s.pov_ok && s.pawn && !s.paused && !s.cursor && host::game_has_focus();
    if (valid) {
        const double motion = g_auto.have_last ? std::abs(wrap180(s.pov_yaw - g_auto.last_yaw)) +
                                                     std::abs(wrap180(s.pov_pitch - g_auto.last_pitch))
                                               : 1;
        if (motion > 0.05 || !g_auto.have_last) g_auto.still_since = now;
        g_auto.last_pitch = s.pov_pitch, g_auto.last_yaw = s.pov_yaw, g_auto.have_last = true;
    } else {
        g_auto.have_last = false;
    }
    const ULONGLONG still_for = valid ? now - g_auto.still_since : 0;
    if (g_test.phase != LiveTest::Off) return run_live_test(s, valid, still_for);
    if (!valid || g_auto.captures >= kMaxAutoCaptures) {
        g_auto.armed = false;
        return;
    }

    if (g_auto.armed) {
        if (look_input()) g_auto.stick_turn = true;
        if (s.pawn != g_auto.pawn || now - g_auto.snapshot_at > 120000) {  // other pawn, or stale
            g_auto.armed = false;
            return;
        }
        // UE5 keeps pitch as 0..360 (350 = 10 degrees down): both deltas wrap.
        const double dy = std::abs(wrap180(s.pov_yaw - g_auto.snap_yaw)), dp = std::abs(wrap180(s.pov_pitch - g_auto.snap_pitch));
        if (still_for < 300 || dy < 30) return;  // still turning, or not far enough yet
        g_auto.armed = false;
        g_auto.next_snapshot = now + 1500;
        // The camera turned without the right stick or the mouse: a cutscene or a scripted camera, not the player's look
        // input.
        if (g_auto.stick_data && !g_auto.stick_turn) {
            probe_log("probe: turn of %.0f degrees ignored: neither the right stick nor the mouse was used (cutscene or "
                      "scripted camera)", dy);
            return;
        }
        ++g_auto.captures;
        probe_log("probe: automatic capture %d: the camera turned %.0f degrees sideways and %.0f up/down", g_auto.captures, dy, dp);
        if (ue::probe_diff(false) == 0) {
            notify::show(L"Gyro4All probe: capture " + std::to_wstring(g_auto.captures) +
                         L" could not be compared (see Gyro4All-probe.log).\nKeep turning with the right stick.");
            return;
        }
        g_auto.target_class = ue::class_of(s.view_target);
        if (g_auto.snap_loc_ok && s.loc_ok) {
            const double angle = view_angle(g_auto.snap_pitch, g_auto.snap_yaw, s.pov_pitch, s.pov_yaw);
            const double chord = distance(g_auto.snap_loc, s.pov_loc);
            const double radius = chord / (2 * std::sin(angle * kDegToRad / 2));
            g_auto.radii.push_back(radius);
            probe_log("probe: the camera moved %.1f units for a %.0f degree turn: orbit radius %.0f (%s)", chord, angle, radius,
                      radius >= kOrbitMin ? "third person, orbits the character" : "turns on the spot");
        }
        if (g_auto.captures >= kCapturesForTest && start_tests()) return;
        if (g_auto.captures >= kMaxAutoCaptures) return finish_tests();
        notify::show(g_auto.captures < kCapturesForTest
                         ? std::format(L"Gyro4All probe: capture {} of {} done.\nTurn the camera again with the RIGHT STICK "
                                       L"or MOUSE (more than 30°, also up or down), then stop.",
                                       g_auto.captures, kCapturesForTest)
                         : std::format(L"Gyro4All probe: capture {} done, no new rotation to test yet.\nTurn the camera again "
                                       L"with the RIGHT STICK or MOUSE (more than 30°), then stop.",
                                       g_auto.captures),
                     10);
        return;
    }
    if (still_for >= 500 && now >= g_auto.next_snapshot) {
        take_snapshot(s);
        g_auto.armed = true;
        g_auto.snap_pitch = s.pov_pitch, g_auto.snap_yaw = s.pov_yaw;
        g_auto.snap_loc_ok = s.loc_ok;
        if (s.loc_ok) std::copy(s.pov_loc, s.pov_loc + 3, g_auto.snap_loc);
        g_auto.stick_turn = false;
        g_auto.pawn = s.pawn;
        g_auto.snapshot_at = now;
        if (!g_auto.announced) {
            g_auto.announced = true;
            log("probe: automatic capture ready: turn the camera with the RIGHT STICK or the MOUSE (more than 30 degrees), "
                "stop, and repeat; after %d captures the rotations found are tested live; the gyro is paused meanwhile",
                kCapturesForTest);
            notify::show(L"Gyro4All probe ready to find the camera. In game, turn the camera with the RIGHT STICK or MOUSE "
                         L"(more than 30°), then stop. Repeat 3 times.\nThen don't touch the controller while "
                         L"the probe tests what it found.",
                         30);
        }
    }
}

bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void keys(const Scene& s) {
    // Plain F7/F8 only: with Ctrl, Shift or Alt they may be the probe key (Ctrl+F7 by default).
    const bool plain = host::game_has_focus() && !key_held(VK_CONTROL) && !key_held(VK_SHIFT) && !key_held(VK_MENU);
    const bool snap = plain && key_held(VK_F7);
    const bool diff = plain && key_held(VK_F8);
    if ((snap || diff) && g_auto.enabled) {
        g_auto.enabled = g_auto.armed = false;
        undo_turn(s);
        g_test.phase = LiveTest::Off;
        log("probe: F7/F8 used, automatic probe (captures and live test) off for this session");
    }
    if (snap && !g_probe_snap_down) take_snapshot(s);
    if (diff && !g_probe_diff_down && ue::probe_diff(true) > 0) notify_manual_diff();
    g_probe_snap_down = snap;
    g_probe_diff_down = diff;
}

// A key setting: F1..F24, optionally after Ctrl+, Shift+ and/or Alt+ (case-insensitive). Empty or unknown = no key.
Key parse_key(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    Key k;
    for (const std::string& part : split(text, '+')) {
        if (part == "CTRL" || part == "CONTROL") k.ctrl = true;
        else if (part == "SHIFT") k.shift = true;
        else if (part == "ALT") k.alt = true;
        else if (part.size() >= 2 && part[0] == 'F' && std::atoi(part.c_str() + 1) >= 1 && std::atoi(part.c_str() + 1) <= 24)
            k.vk = VK_F1 + std::atoi(part.c_str() + 1) - 1;
        else return {};
    }
    if (!k.vk) return {};
    k.name = std::string(k.ctrl ? "Ctrl+" : "") + (k.shift ? "Shift+" : "") + (k.alt ? "Alt+" : "") + "F" +
             std::to_string(k.vk - VK_F1 + 1);
    return k;
}

// ProbeKey= as written: the game's profile wins over Gyro4All.ini, [Gyro4All] over [GyroUE]. Present but empty = no
// key (settings::get_string can't tell that from missing); missing everywhere = Ctrl+F7.
std::string probe_key_setting() {
    for (const std::wstring* path : {&settings::profile_path(), &settings::settings_path()})
        for (const wchar_t* section : {settings::kSection, settings::kLegacySection}) {
            wchar_t value[64];
            GetPrivateProfileStringW(section, L"ProbeKey", L"\x01", value, 64, path->c_str());
            if (value[0] != L'\x01') return narrow(value);
        }
    return "Ctrl+F7";
}

// Gyro4All-probe.log, opened when the probe first runs this session (at start with Probe=1, or later from the probe
// key or the automatic start).
bool open_probe_log() {
    static bool opened = false, tried = false;
    if (tried) return opened;
    tried = true;
    const std::wstring probe_path = settings::mod_dir() + L"\\Gyro4All-probe.log";
    opened = probe_log_open(probe_path, probe_header());
    log("probe: results in %s", opened ? narrow(probe_path).c_str() : "Gyro4All.log (probe file failed)");
    return opened;
}

// Starts the probe during play, for the camera of that moment: from the probe key, or by itself (AutoProbe) when the
// camera stopped following the gyro. Earlier captures and verdicts are forgotten.
void probe_start(bool automatic) {
    if (g_probe) return;
    open_probe_log();
    ue::probe_reset();
    const ULONGLONG keep_notice = g_test.last_pause_notice;
    g_auto = {};
    g_test = {};
    g_test.last_pause_notice = keep_notice;
    g_probe = true;
    g_probe_runtime = true;
    g_probe_automatic = automatic;
    g_probe_started_at = GetTickCount64();
    const char* why = automatic ? "AutoProbe: the camera stopped following the gyro" : "probe key";
    log("probe: started during play (%s)", why);
    probe_log("probe: started during play (%s)", why);
    if (automatic) {
        g_auto.announced = true;  // this notice replaces the usual "probe ready" one
        const std::wstring cancel = g_probe_key.name.empty()
                                        ? std::wstring()
                                        : L" " + std::wstring(g_probe_key.name.begin(), g_probe_key.name.end()) + L" cancels.";
        notify::show(L"Gyro4All: this camera ignores the gyro. To teach it to Gyro4All, turn the camera with the RIGHT "
                     L"STICK or MOUSE (more than 30°), then stop. Repeat 3 times.\nThen don't touch the controller for a "
                     L"few seconds." + cancel,
                     20);
    }
}

}  // namespace

const char* probe_default_settings() { return kProbeSettings; }

void probe_configure(bool profile_existed) {
    g_profile_existed = profile_existed;
    g_probe = settings::get(L"Probe", 0) != 0;
    g_auto_probe_setting = settings::get(L"AutoProbe", 1) != 0;
    g_probe_key = parse_key(probe_key_setting());
    log("settings: Probe=%d AutoProbe=%d ProbeKey=%s (Gyro4All.ini, the game's profile overrides)", g_probe ? 1 : 0,
        g_auto_probe_setting ? 1 : 0, g_probe_key.name.empty() ? "(none)" : g_probe_key.name.c_str());
    if (g_probe) {
        open_probe_log();
        log("profile: probe enabled (automatic capture on stick or mouse turns, F7/F8 manual)");
    }
}

bool probe_on() { return g_probe; }

// While the automatic probe runs, the gyro output is paused: a turn must come from the right stick or the mouse alone,
// and the gyro written into a rotation the camera ignores (or a resting controller's drift) would only spoil it.
// An automatic probe (AutoProbe) keeps the gyro on until its live test: the camera ignores it anyway, a turn without the
// stick or mouse isn't captured, and the follow check must see it when the camera follows the gyro again.
bool probe_capturing() {
    return g_probe && g_auto.enabled && !g_auto.done && (!g_probe_automatic || g_test.phase != LiveTest::Off);
}

void probe_stick_seen() { g_auto.stick_data = true; }

void probe_keys(const Scene& s) { keys(s); }
void auto_probe(const Scene& s) { automatic(s); }

void probe_hotkey(const Scene& s) {
    const Key& k = g_probe_key;
    const bool down = k.vk && host::game_has_focus() && key_held(k.vk) && k.ctrl == key_held(VK_CONTROL) &&
                      k.shift == key_held(VK_SHIFT) && k.alt == key_held(VK_MENU);
    if (down && !g_probe_key_down) {
        if (!g_probe) {
            probe_start(false);
        } else {
            undo_turn(s);
            probe_end("probe key");
            notify::show(L"Gyro4All probe cancelled.", 5);
        }
    }
    g_probe_key_down = down;
}

void auto_probe_watch() {
    if (!g_probe || !g_probe_automatic || g_test.phase != LiveTest::Off) return;
    const ULONGLONG now = GetTickCount64();
    if (g_auto.captures == 0 && now - g_probe_started_at > 45000) {
        probe_end("AutoProbe: no camera turn with the stick or mouse in 45 s");
        notify::show(L"Gyro4All: stopped looking for the camera (no turn with the stick or mouse).", 6);
    }
}

bool request_auto_probe() {
    if (!g_auto_probe_setting || g_probe || g_auto_probe_starts >= 3 || GetTickCount64() < g_next_auto_probe) return false;
    ++g_auto_probe_starts;
    probe_start(true);
    return true;
}

void auto_probe_camera_follows() {
    if (g_probe && g_probe_automatic && g_test.phase == LiveTest::Off) probe_end("AutoProbe: the camera follows the gyro again");
}

}  // namespace gyro4all::unreal
