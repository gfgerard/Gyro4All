// Decima engine module: Death Stranding (and its Director's Cut), Horizon Zero Dawn (PC builds).
//
// Detection: the exe's name, with the engine's data layout next to it (a data\ folder of hashed .bin archives).
// Without it no engine matched and the Unreal fallback scanned the game's memory: its guarded reads fault, and
// Death Stranding's crash reporter (a vectored exception handler) sees the first-chance access violation, writes
// error.txt and shows an "Access Violation" window although Gyro4All handled it.
//
// Camera (Death Stranding Director's Cut, MODLOG 2026-11-15): the active camera mode, as the game's own getter finds it
// (the function its script export SetCameraRotationHeading's consumer calls): a global list of players, the one whose
// id is 0, then three pointers. The mode keeps the view's pitch and heading as float radians, read and written by the
// camera's update; adding to them turns the view (pitch: + up; heading: + left). The global, the
// pointer offsets and the angle offsets are read from the game's code by signature, so other builds can match.
#include "decima.hpp"

#include "engine.hpp"
#include "fov.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gyro4all::decima {

namespace {

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool has_hashed_data(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\data\\*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

bool decima_layout(const std::wstring& exe_path) {
    const size_t slash = exe_path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"." : exe_path.substr(0, slash);
    std::wstring stem = lower(slash == std::wstring::npos ? exe_path : exe_path.substr(slash + 1));
    if (const size_t dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
    static const wchar_t* const stems[] = {L"deathstranding", L"horizonzerodawn"};
    return std::any_of(std::begin(stems), std::end(stems), [&](const wchar_t* s) { return stem == s; }) &&
           has_hashed_data(dir);
}

// The camera mode getter: mov rcx,[rip+G]; ... call players_find(G, 0); [rax+A]; [rax+B]-0x20; [rcx+C]-0x20.
constexpr const char* kModeGetter =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 33 D2 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 40 ?? 48 85 C0 74 ?? "
    "48 8B 80 ?? ?? ?? ?? 33 D2 48 85 C0 48 8D 48 E0 48 0F 44 CA 48 85 C9 74 ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9 48 8D 41 E0";
// The rotation request's consumer: movzx ecx,[rbx+flags]; vmovss xmm3,[rax+pitch]; vmovss xmm4,[rax+heading]; test cl,20h.
constexpr const char* kModeAngles = "0F B6 8B ?? ?? ?? ?? C5 FA 10 98 ?? ?? ?? ?? C5 FA 10 A0 ?? ?? ?? ?? F6 C1 20";

struct Layout {
    uintptr_t players = 0;       // the global holding the player list
    uint32_t a = 0, b = 0, c = 0;  // player -> +a -> +b (-0x20) -> +c (-0x20) = the camera mode
    uint32_t pitch = 0, heading = 0;
};

std::vector<uintptr_t> find(const char* pattern) {
    std::vector<uint8_t> bytes, mask;
    mem::parse_pattern(pattern, bytes, mask);
    return mem::find_pattern(mem::code_sections(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))), bytes.data(),
                             mask.data(), bytes.size());
}

template <class T>
T at(uintptr_t p) {
    T v{};
    mem::rd(p, v);
    return v;
}

void read_getter(uintptr_t g, Layout& l) {
    l.players = g + 11 + at<int32_t>(g + 7);
    l.a = at<uint8_t>(g + 31);
    l.b = at<uint32_t>(g + 40);
    l.c = at<uint32_t>(g + 65);
}

bool find_layout(Layout& l) {
    const auto getters = find(kModeGetter);
    const auto angles = find(kModeAngles);
    // The getter exists in several identical copies (Death Stranding DC: two): fine as long as they all agree.
    bool agree = !getters.empty();
    if (agree) read_getter(getters[0], l);
    for (size_t i = 1; agree && i < getters.size(); ++i) {
        Layout o;
        read_getter(getters[i], o);
        agree = o.players == l.players && o.a == l.a && o.b == l.b && o.c == l.c;
    }
    if (!agree || angles.size() != 1) {
        log("decima: camera code not found (mode getter %zu match(es)%s, angle reads %zu): gyro off", getters.size(),
            getters.size() > 1 && !agree ? " that differ" : "", angles.size());
        return false;
    }
    const uintptr_t g = getters[0], an = angles[0];
    l.pitch = at<uint32_t>(an + 11);
    l.heading = at<uint32_t>(an + 19);
    log("decima: camera mode getter %s (players %s, +0x%x +0x%x +0x%x), angles +0x%x pitch +0x%x heading (%s)",
        mem::module_offset(g).c_str(), mem::module_offset(l.players).c_str(), l.a, l.b, l.c, l.pitch, l.heading,
        mem::module_offset(an).c_str());
    return l.a && l.b && l.c && l.pitch && l.heading;
}

// The game's getter, with guarded reads: 0 while there is no player or camera mode (loading, front end).
uintptr_t camera_mode(const Layout& l) {
    const uintptr_t list = mem::rdp(l.players);
    if (!list) return 0;
    const uintptr_t items = mem::rdp(list + 0x40);
    int32_t count = 0;
    if (!items || !mem::rd(list + 0x38, count) || count <= 0 || count > 64) return 0;
    uintptr_t player = 0;
    for (int32_t i = 0; i < count && !player; ++i) {
        const uintptr_t p = mem::rdp(items + 8 * static_cast<uintptr_t>(i));
        int32_t id = -1;
        if (p && mem::rd(p + 0x88, id) && id == 0) player = p;
    }
    if (!player) return 0;
    const uintptr_t x = mem::rdp(player + l.a);
    const uintptr_t y = x ? mem::rdp(x + l.b) : 0;
    if (!y) return 0;
    const uintptr_t z = mem::rdp(y - 0x20 + l.c);
    return z ? z - 0x20 : 0;
}

constexpr double kRad = 3.14159265358979323846 / 180.0;

// The render view's FOV (Death Stranding DC, found by a zoom diff; MODLOG 2026-11-15): view objects a few pointers from
// the camera mode hold an inverse projection where tan(vertical FOV / 2) sits 0x40 bytes after its reciprocal and
// 0x28 bytes after tan(horizontal FOV / 2) (third person 45.7 degrees, first person 45.3, the binoculars' R3 zoom 23.3).
// The pointer path between the mode and those objects changes from one run to the next, so they are found by that
// pattern, walking the pointers out of the mode.
//
// That walk follows arbitrary pointers, so it reads with ReadProcessMemory, which fails without an exception: a
// guarded read that faults is caught by Gyro4All, but the game's crash reporter sees the first-chance access violation
// and shows its "Access Violation" window.
bool quiet_read(uintptr_t at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}

// t at `p` (tan of half the vertical FOV, below 87 degrees), with 1/t at p - 0x40 and tan of half the horizontal FOV at
// p - 0x28, in the game window's aspect ratio (within 3%). Without the aspect check, square views (shadow or cube
// map projections, 90 degrees while loading) match too.
bool fov_pattern(const float* p, double aspect) {
    const float t = p[0], inv = p[-16], h = p[-10];
    if (!std::isfinite(t) || t < 0.05f || t > 0.95f || std::fabs(inv * t - 1.f) > 1e-3f) return false;
    return aspect > 0 && std::fabs(h / t / aspect - 1) < 0.03;
}

// Breadth-first through the pointers of the mode (3 levels), the first object holding the pattern: its t's address.
uintptr_t find_fov(uintptr_t mode, double aspect) {
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<std::pair<uintptr_t, size_t>> level{{mode, 0x2000}}, next;
    std::unordered_set<uintptr_t> seen;
    std::vector<uint8_t> buf;
    for (int depth = 0; depth <= 3 && !level.empty(); ++depth) {
        next.clear();
        for (const auto& [obj, size] : level) {
            buf.resize(size);
            if (!quiet_read(obj, buf.data(), size)) continue;
            const float* f = reinterpret_cast<const float*>(buf.data());
            for (size_t i = 16; i < size / 4; ++i)
                if (fov_pattern(f + i, aspect)) return obj + 4 * i;
            if (depth == 3) continue;
            for (size_t o = 0; o + 8 <= size; o += 8) {
                uintptr_t v = 0;
                std::memcpy(&v, buf.data() + o, 8);
                if (!mem::plausible(v) || (v >= exe && v < exe + 0x10000000)) continue;
                if (!seen.insert(v).second) continue;
                if (seen.size() >= 20000) return 0;

                next.emplace_back(v, 0x800);
            }
        }
        level.swap(next);
    }
    return 0;
}

// The vertical FOV in degrees from the pattern at `at`, 0 if it's no longer there.
double vertical_fov(uintptr_t at, double aspect) {
    float p[17];
    if (!at || !quiet_read(at - 0x40, p, sizeof p) || !fov_pattern(p + 16, aspect)) return 0;
    const double fov = 2 * std::atan(static_cast<double>(p[16])) / kRad;
    return fov::is_valid(fov) ? fov : 0;
}

class DecimaEngine final : public Engine {
public:
    const char* name() const override { return "Decima"; }
    bool detect(const std::wstring& exe_path) const override { return decima_layout(exe_path); }
    void configure() override { load_zoom_reference(); }
    void settings_changed() override { load_zoom_reference(); }
    std::vector<ViewInfo> views() const override { return {{1, "Default", 0, true}}; }

    EngineFrame read_frame() override {
        EngineFrame f;
        if (!searched_) {
            searched_ = true;
            found_ = find_layout(layout_);
        }
        mode_ = found_ ? camera_mode(layout_) : 0;
        float pitch = 0, heading = 0;
        if (mode_ && (!mem::rd(mode_ + layout_.pitch, pitch) || !mem::rd(mode_ + layout_.heading, heading) ||
                      !std::isfinite(pitch) || !std::isfinite(heading) || std::fabs(pitch) > 3.2f))
            mode_ = 0;
        if (mode_ != logged_mode_) {
            logged_mode_ = mode_;
            if (mode_) log("decima: camera mode %s (pitch %.1f, heading %.1f degrees)", mem::module_offset(mode_).c_str(),
                           pitch / kRad, heading / kRad);
            else log("decima: no camera mode");
        }
        if (!mode_) return f;
        f.in_game = true;
        f.view_id = 1;
        f.target = "camera mode";
        const double aspect = fov::game_window_aspect();
        double fov = vertical_fov(fov_at_, aspect);
        if (!fov && (mode_ != fov_mode_ || GetTickCount64() >= next_fov_search_)) {
            const ULONGLONG t0 = GetTickCount64();
            fov_mode_ = mode_;
            fov_at_ = find_fov(mode_, aspect);
            next_fov_search_ = GetTickCount64() + 2000;
            fov = vertical_fov(fov_at_, aspect);
            if (fov_searches_ < 20) {
                ++fov_searches_;
                log("decima: FOV %s (%.1f degrees vertical, window aspect %.3f, search %llu ms)",
                    fov ? ("at " + mem::module_offset(fov_at_)).c_str() : "not found", fov, aspect, GetTickCount64() - t0);
            }
        }
        f.zoom_fov = zoom_fov(fov);
        return f;
    }

    // Degrees from GyroLib (pitch up, yaw right) added to the mode's radians: pitch up is +, heading right is - (tested).
    bool apply(double pitch, double yaw) override {
        if (!mode_) return false;
        float p = 0, h = 0;
        if (!mem::rd(mode_ + layout_.pitch, p) || !mem::rd(mode_ + layout_.heading, h)) return false;
        p = static_cast<float>(p + pitch * kRad);
        h = static_cast<float>(h - yaw * kRad);
        return mem::guarded_store(mode_ + layout_.pitch, &p, sizeof p) &&
               mem::guarded_store(mode_ + layout_.heading, &h, sizeof h);
    }

    bool sees_menus() const override { return false; }
    std::string status_scene() const override {
        if (!found_) return searched_ ? "view=none (camera code not found)" : "view=none";
        return mode_ ? "view=default (camera mode " + mem::module_offset(mode_) + ")" : "view=none (no camera mode)";
    }

private:
    // [Gyro4All] ZoomReferenceFOV= (horizontal degrees); 0 learns the unzoomed FOV: the widest one seen.
    void load_zoom_reference() {
        const double fov = fov::reference_setting();
        if (fov != zoom_reference_fov_)
            log("decima: zoom reference FOV %s", fov > 0 ? (std::to_string(int(fov)) + " degrees horizontal").c_str()
                                                         : "learnt in game (the widest FOV seen)");
        zoom_reference_fov_ = fov;
    }

    std::optional<ZoomFov> zoom_fov(double fov) {
        if (!fov::is_valid(fov)) return std::nullopt;
        if (fov > widest_ + 0.05) {
            widest_ = fov;
            log("decima: widest FOV seen %.1f degrees vertical", widest_);
        }
        ZoomFov z;
        z.current_vertical = fov;
        z.reference_vertical = zoom_reference_fov_ > 0
                                   ? fov::vertical_from_horizontal(zoom_reference_fov_, fov::game_window_aspect())
                                   : widest_;
        if (!fov::is_valid(z.reference_vertical)) return std::nullopt;
        if (std::fabs(fov - logged_fov_) > 1 && fov_logs_ < 50) {
            ++fov_logs_;
            logged_fov_ = fov;
            log("decima: FOV %.1f degrees vertical (unzoomed %.1f)", z.current_vertical, z.reference_vertical);
        }
        return z;
    }

    double zoom_reference_fov_ = -1, widest_ = 0, logged_fov_ = 0;
    int fov_logs_ = 0, fov_searches_ = 0;
    uintptr_t fov_at_ = 0, fov_mode_ = 0;
    ULONGLONG next_fov_search_ = 0;
    Layout layout_;
    bool searched_ = false, found_ = false;
    uintptr_t mode_ = 0, logged_mode_ = 0;
};

}  // namespace

Engine& engine() {
    static DecimaEngine e;
    return e;
}

}  // namespace gyro4all::decima
