// The Gyro4All settings window, next to GyroLib's panel (see settings_ui.hpp). One window, two states:
// folded = a button with a drawn cog icon in the bottom-left of GyroLib's panel footer, on the "Reset all settings"
// row; expanded = a window the size of GyroLib's panel, exactly over it (its × button folds the window back). It uses
// with a blue TitleBgActive header bar 44*scale tall (frame 18*s + 2*5*s padding, +16*s), title at 18*scale and a
// close button, then content at (22*s, bar+18*s) (its draw_header layout, from the MIT-licensed GyroLib source). The
// UI uses GyroLib's own scaling recipe: scale = clamp(max(1, dpi, height/1080), 0.75, max(0.75, min(3, height/760))),
// PushFont(nullptr, 18*scale), and the panel's exact centred geometry (min(w-24s, 960s) x min(h-24s, 870s)) and footer
// layout (footer_height = calibration row + action row + 12*s, so the action row's centre is ~32*s above the window's
// bottom edge) — so the button lands on the reset row wherever GyroLib put it, also in DX12 games, where its window is
// drawn inside gyrolib.dll and invisible to this DLL. Everything is applied locally (style vars pushed for this window
// only), so the shared ImGui context and GyroLib's panel are untouched. Edits are written to Gyro4All.ini as they
// happen; each setting's help says whether it applies on save or needs a restart (host.cpp: the pacer, the stick
// camera and zoom compensation reload live via the settings-file watcher; everything else is read in host_configure at
// start).
#include "settings_ui.hpp"

#include <gyrolib/gyrolib.h>
#include <imgui.h>
#include <imgui_internal.h>  // BringWindowToDisplayFront (this ImGui keeps it internal)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace g4a::settings_ui {
namespace {

const g4a_settings* g_settings = nullptr;
gl_context* g_ctx = nullptr;  // GyroLib's context: per-controller button naming and the selected device
bool g_expanded = false;
uint32_t g_event = G4A_SETTINGS_NONE;  // the last expand/fold/close, until winmm.dll takes it (take_event)
bool g_focus_pending = false;    // a chord just unfolded the window: move gamepad-nav focus into it
bool g_restore_pending = false;  // the window just folded: return nav focus to GyroLib's panel
float g_dpi = 1.0f;
float g_s = 1.0f;  // the current UI scale, for helpers that draw next to widgets

ImVec2 scaled(ImVec2 v, float s) { return ImVec2(v.x * s, v.y * s); }

// GyroLib's panel scale (src/panel.cpp gl_panel_draw): the larger of 1, DPI and height/1080, capped so small
// windows shrink a little and portrait ones never explode.
float ui_scale(float viewport_height) {
    float s = std::max({1.0f, g_dpi, viewport_height / 1080.0f});
    return std::clamp(s, 0.75f, std::max(0.75f, std::min(3.0f, viewport_height / 760.0f)));
}

// GyroLib's panel rectangle in this viewport: centred, min(w-24s, 960s) x min(h-24s, 870)s, and its footer geometry.
struct PanelBox {
    ImVec2 center;
    float left, top, width, height;  // rectangle edges
    float scale;
};
PanelBox panel_box(const ImGuiViewport* vp) {
    const float s = ui_scale(vp->WorkSize.y);
    const float w = std::min(vp->WorkSize.x - 24.0f * s, 960.0f * s);
    const float h = std::min(vp->WorkSize.y - 24.0f * s, 870.0f * s);
    const float left = vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f;
    const float top = vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f;
    return {{left + w * 0.5f, top + h * 0.5f}, left, top, w, h, s};
}

// Where the folded button's window goes: on the footer's action row ("Reset all settings"), left-aligned. That row's
// centre is ~32*s above the window's bottom edge (bottom padding 18*s + half the 28*s frame); our button is ~40*s
// tall, so its top is ~52*s above the bottom. x: the window's left padding inset, like the footer's content.
ImVec2 button_pos(const PanelBox& p) { return ImVec2(p.left + 22.0f * p.scale, p.top + p.height - 52.0f * p.scale); }

// GyroLib's window style sizes (gl_panel_draw): these three, then the remaining sizes scaled from the defaults.
struct ScopedScale {
    explicit ScopedScale(float s) {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * s, 18.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * s, 5.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * s, 9.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, scaled(st.ItemInnerSpacing, s));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, st.WindowRounding * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, st.FrameRounding * s);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, st.GrabRounding * s);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, st.GrabMinSize * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, st.ScrollbarSize * s);
    }
    ~ScopedScale() { ImGui::PopStyleVar(9); }
};

// A cog (gear) icon drawn, not text: ImGui's default font has no U+2699, and this looks the same in every renderer.
void cog(ImDrawList* draw, ImVec2 centre, float r, float thick, ImU32 colour) {
    draw->AddCircle(centre, r * 0.60f, colour, 0, thick);
    draw->AddCircle(centre, r * 0.16f, colour, 0, thick);
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 3.14159265358979323846f / 4.0f;
        const float c = std::cos(a), s = std::sin(a);
        draw->AddLine(ImVec2(centre.x + c * r * 0.68f, centre.y + s * r * 0.68f),
                      ImVec2(centre.x + c * r * 1.06f, centre.y + s * r * 1.06f), colour, thick);
    }
}

std::string get(const char* key, const char* fallback = "") {
    char buffer[256];
    if (g_settings && g_settings->get(key, buffer, sizeof buffer)) return buffer;
    return fallback;
}

int get_int(const char* key, int fallback) { return std::atoi(get(key, std::to_string(fallback).c_str()).c_str()); }

}  // namespace

namespace {

float get_float(const char* key, float fallback) {
    return float(std::atof(get(key, std::to_string(fallback).c_str()).c_str()));
}

void set(const char* key, const std::string& value) {
    if (g_settings) g_settings->set(key, value.c_str());
}

// Defined below (after `section`): the undo-arrow reset button drawn next to non-default settings.
bool reset_arrow(const char* key, bool dirty, const std::string& default_value);

void checkbox(const char* label, const char* key, int fallback = 0) {
    bool value = get_int(key, fallback) != 0;
    if (ImGui::Checkbox(label, &value)) set(key, value ? "1" : "0");
    reset_arrow(key, value != (fallback != 0), fallback ? "1" : "0");
}

// Sliders bound to int/float settings; `fallback` is the default used when the INI has no value (the class's own
// defaults, so the slider shows the same value the game runs with).
void slider_int(const char* label, const char* key, int min, int max, int fallback) {
    int value = get_int(key, fallback);
    if (ImGui::SliderInt(label, &value, min, max)) set(key, std::to_string(value));
    reset_arrow(key, value != fallback, std::to_string(fallback));
}

void slider_float(const char* label, const char* key, float min, float max, float fallback, const char* fmt = "%.2f") {
    float value = get_float(key, fallback);
    if (ImGui::SliderFloat(label, &value, min, max, fmt)) {
        char text[32];
        std::snprintf(text, sizeof text, fmt, double(value));
        set(key, text);
    }
    char def[32];
    std::snprintf(def, sizeof def, "%g", double(fallback));
    reset_arrow(key, value != fallback, def);
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A section title with generous margins around it, so the blocks read apart: bold (the default font has no bold face:
// the title is drawn again 0.75*s to the right) and underlined, and the section's content
// indented under it until the next section (end_sections closes the last one).
bool g_in_section = false;
constexpr float kSectionIndent = 25.0f;

void end_sections(float s) {
    if (g_in_section) ImGui::Unindent(kSectionIndent * s);
    g_in_section = false;
}

void section(const char* title, float s) {
    end_sections(s);
    ImGui::Dummy(ImVec2(0, 10.0f * s));
    ImGui::SeparatorText(title);
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 at(min.x + style.SeparatorTextPadding.x, min.y + style.SeparatorTextPadding.y);
    const ImVec2 size = ImGui::CalcTextSize(title, nullptr, true);
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(ImVec2(at.x + 0.75f * s, at.y), col, title);
    const float underline = at.y + size.y + 1.0f * s;
    draw->AddLine(ImVec2(at.x, underline), ImVec2(at.x + size.x + 0.75f * s, underline), col, 1.0f * s);
    ImGui::Dummy(ImVec2(0, 5.0f * s));
    ImGui::Indent(kSectionIndent * s);
    g_in_section = true;
}

// An undo-arrow button to the right of a setting whose value differs from its default; clicking resets the
// setting to the default (written to the INI, so it persists). The arrow is drawn (arc + head), not text: ImGui's
// font has no U+21B6. Returns true when clicked.
bool reset_arrow(const char* key, bool dirty, const std::string& default_value) {
    if (!dirty) return false;
    ImGui::SameLine(0, 6.0f * g_s);
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(key);
    const bool clicked = ImGui::Button("##g4a_reset", ImVec2(h, h));
    ImGui::PopID();
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    constexpr float kPi = 3.14159265358979323846f;
    const float r = h * 0.26f, thick = 2.0f * g_s;
    // An undo-style circular arrow, mirrored so it reads leftward: the arc runs from the bottom
    // counter-clockwise (ImGui angles grow clockwise on screen, so the sweep is decreasing) almost a full turn,
    // ending at the lower left; the head extends past the arc's end along the travel direction there.
    const float a0 = 0.5f * kPi, a1 = -1.05f * kPi;
    draw->PathArcTo(c, r, a0, a1, 24);
    draw->PathStroke(col, 0, thick);
    const float ca = std::cos(a1), sa = std::sin(a1);
    const ImVec2 t(sa, -ca), n(-t.y, t.x);  // travel direction (a decreasing) and its perpendicular
    const float hl = h * 0.22f, hw = h * 0.15f;
    const ImVec2 tip(c.x + ca * r + t.x * hl * 0.6f, c.y + sa * r + t.y * hl * 0.6f);
    draw->AddTriangleFilled(tip, ImVec2(tip.x - t.x * hl + n.x * hw, tip.y - t.y * hl + n.y * hw),
                            ImVec2(tip.x - t.x * hl - n.x * hw, tip.y - t.y * hl - n.y * hw), col);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset to default (%s)", default_value.empty() ? "empty" : default_value.c_str());
    if (clicked) set(key, default_value);
    return clicked;
}

// The blue header bar like GyroLib's draw_header: TitleBgActive over the window's top, rounded top corners, title at
// 18*s on the left, a close button at the right. Returns false when closed (the caller folds and moves nav focus
// back to GyroLib's panel).
bool header(const char* title, float s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float bar = ImGui::GetFrameHeight() + 16.0f * s;
    const ImVec2 origin = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + bar),
                                              ImGui::GetColorU32(ImGuiCol_TitleBgActive), style.WindowRounding,
                                              ImDrawFlags_RoundCornersTop);
    const float inset = 22.0f * s;
    ImGui::SetCursorPos(ImVec2(inset, 8.0f * s));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    const float close_width = 28.0f * s;
    ImGui::SameLine(ImGui::GetWindowWidth() - inset - close_width);
    if (ImGui::Button("×##g4a_close", ImVec2(close_width, 0))) return false;
    ImGui::SetCursorPos(ImVec2(inset, bar + 18.0f * s));
    return true;
}

// A combo bound to a string setting: `values` are written to the INI, `labels` are shown ("(disabled)" for "").
void combo(const char* label, const char* key, const char* const* values, const char* const* labels, int count) {
    const std::string value = get(key);
    int selected = 0;
    for (int i = 0; i < count; ++i)
        if (value == values[i]) selected = i;
    if (ImGui::Combo(label, &selected, labels, count)) set(key, values[selected]);
    reset_arrow(key, !value.empty(), "");
}

// The activation-button entries, position-based: `ordinal` is the SDL button (100/101 = the triggers, which SDL
// reports as axes, not buttons). The value written to the INI and the label shown depend on the connected
// controller's type — the same two naming sets the INI documents (LT/A/X... or L2/Cross/Square...); both parse to
// the same ordinals in host.cpp's parse_button, so a stored value from either set works with either pad.
struct ButtonEntry {
    int ordinal;             // SDL_GamepadButton, 100 = LT/L2, 101 = RT/R2, -1 = the (disabled) entry
    const char* xbox_value;
    const char* ps_value;
};
constexpr ButtonEntry kButtonEntries[] = {
    {-1, "", ""},
    {100, "LT", "L2"},   {101, "RT", "R2"},   {9, "LB", "L1"},     {10, "RB", "R1"},
    {7, "LS", "L3"},     {8, "RS", "R3"},     {0, "A", "Cross"},   {1, "B", "Circle"},
    {2, "X", "Square"},  {3, "Y", "Triangle"},
    {4, "Back", "Back"}, {6, "Start", "Start"},
    {11, "Up", "Up"},    {12, "Down", "Down"}, {13, "Left", "Left"}, {14, "Right", "Right"},
    {20, "Touchpad", "Touchpad"}, {15, "Misc", "Misc"},
    {17, "LP1", "LP1"},  {16, "RP1", "RP1"},  {19, "LP2", "LP2"},  {18, "RP2", "RP2"},
};
constexpr int kButtonEntryCount = int(sizeof(kButtonEntries) / sizeof(kButtonEntries[0]));

// A stored INI value -> its ordinal (both documented naming sets; host.cpp's parse_button is the authority).
int button_ordinal(std::string name) {
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    static const struct {
        const char* name;
        int ordinal;
    } kNames[] = {
        {"LT", 100}, {"L2", 100}, {"RT", 101}, {"R2", 101}, {"LB", 9},   {"L1", 9},   {"RB", 10}, {"R1", 10},
        {"LS", 7},   {"L3", 7},   {"RS", 8},   {"R3", 8},   {"A", 0},    {"CROSS", 0}, {"B", 1},  {"CIRCLE", 1},
        {"X", 2},    {"SQUARE", 2}, {"Y", 3},  {"TRIANGLE", 3}, {"BACK", 4}, {"VIEW", 4}, {"CREATE", 4},
        {"START", 6}, {"MENU", 6}, {"OPTIONS", 6}, {"UP", 11}, {"DOWN", 12}, {"LEFT", 13}, {"RIGHT", 14},
        {"TOUCHPAD", 20}, {"MISC", 15}, {"MISC1", 15}, {"LP1", 17}, {"RP1", 16}, {"LP2", 19}, {"RP2", 18},
    };
    for (const auto& n : kNames)
        if (name == n.name) return n.ordinal;
    return -1;
}

// Is the selected controller a PlayStation pad? gl_endpoint exposes the name only; the SDL names cover the
// DualSense, DualShock 4 and generic "PS4/PS5 Controller" entries.
bool playstation_pad() {
    if (!g_ctx) return false;
    const uint64_t selected = gl_get_selected_device(g_ctx);
    for (uint32_t i = 0; i < gl_endpoint_count(g_ctx); ++i) {
        gl_endpoint e{};
        if (gl_get_endpoint(g_ctx, i, &e) != GL_OK || !e.connected) continue;
        if (selected && e.physical_id != selected) continue;
        std::string name = e.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool ps = name.find("dualsense") != std::string::npos || name.find("dualshock") != std::string::npos ||
                        name.find("playstation") != std::string::npos || name.find("ps4") != std::string::npos ||
                        name.find("ps5") != std::string::npos;
        if (selected) return ps;  // the selected endpoint is the one whose labels the user thinks in
        if (ps) return true;
    }
    return false;
}

// A button setting's combo (the activation-button entries), `width` wide plus its reset arrow.
void button_combo(const char* key, float width) {
    // Persistent storage refilled every frame: a combo popup stays open across frames and reads these pointers
    // while it is. The label/value set follows the connected controller's type.
    static std::array<std::string, kButtonEntryCount> labels;
    static const char* label_ptrs[kButtonEntryCount];
    static const char* values[kButtonEntryCount];
    const bool ps = playstation_pad();
    for (int i = 0; i < kButtonEntryCount; ++i) {
        values[i] = ps ? kButtonEntries[i].ps_value : kButtonEntries[i].xbox_value;
        labels[size_t(i)] = kButtonEntries[i].ordinal < 0 ? "(activation button)" : values[i];
        label_ptrs[i] = labels[size_t(i)].c_str();
    }
    // Select by ordinal: an INI value written with the other naming set (LT on a PlayStation pad) still shows.
    const int want = button_ordinal(get(key));
    int selected = 0;
    for (int i = 0; i < kButtonEntryCount; ++i)
        if (want >= 0 && kButtonEntries[i].ordinal == want) selected = i;
    ImGui::PushID(key);
    ImGui::SetNextItemWidth(width);
    if (ImGui::Combo("##button", &selected, label_ptrs, kButtonEntryCount)) set(key, values[selected]);
    ImGui::PopID();
    reset_arrow(key, want >= 0, "");
}

// A label and its help, then a button combo and a Hold/Toggle combo on one row.
void button_mode_row(const char* id, const char* label, const char* help_text, const char* button_key,
                     const char* mode_key, float s) {
    static const char* kModes[] = {"", "Hold", "Toggle"};
    static const char* kModeLabels[] = {"(needs a button)", "Hold", "Toggle"};
    ImGui::PushID(id);  // the rows share their visible labels: scope the IDs per row
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    help(help_text);
    ImGui::SameLine();
    button_combo(button_key, ImGui::GetContentRegionAvail().x * 0.5f - 6.0f * s);
    ImGui::SameLine();
    // The mode combo leaves room for its reset arrow when the value is not the default (empty).
    const float reset_room = get(mode_key).empty() ? 0.0f : ImGui::GetFrameHeight() + 12.0f * s;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset_room);
    combo("##mode", mode_key, kModes, kModeLabels, IM_ARRAYSIZE(kModes));
    ImGui::PopID();
}

void profile_row(const char* id, const char* label, const char* button_key, const char* mode_key, float s) {
    button_mode_row(id, label,
                    "Extra sensitivity tab in the F10 panel (starts with the Exploration/ADS settings).\n"
                    "A profile needs both its button and its mode. Restart the game to apply.",
                    button_key, mode_key, s);
}

// A free-text setting (comma lists), committed when the box loses focus or Enter is pressed. The buffer is read from
// the INI once; resetting also clears it.
void text_setting(const char* key, const char* hint) {
    struct Buffer {
        char text[128] = "";
        bool loaded = false;
    };
    static std::map<std::string, Buffer> buffers;
    Buffer& b = buffers[key];
    if (!b.loaded) {
        b.loaded = true;
        std::snprintf(b.text, sizeof b.text, "%s", get(key).c_str());
    }
    ImGui::PushID(key);
    const bool enter = ImGui::InputTextWithHint("##text", hint, b.text, sizeof b.text, ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || ImGui::IsItemDeactivatedAfterEdit()) set(key, b.text);
    ImGui::PopID();
    if (reset_arrow(key, !get(key).empty(), "")) b.loaded = false;
}

void widgets(float s) {
    // Defaults (the fallbacks) are StickCamera::Settings' own and the shipped Gyro4All.ini defaults, so a slider
    // shows exactly what the game runs with when the INI has no value.
    section("Gyro off while held", s);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Buttons");
    help("GyroOffButtons: held over 200 ms, the gyro stops turning the camera until they are released\n"
         "(a weapon wheel the game opens on a held button). A short press (switching weapons) doesn't stop it.\n"
         "Names: LT RT LB RB LS RS A B X Y (or L2 R2 L1 R1 L3 R3 Cross Circle Square Triangle), Back Start\n"
         "Up Down Left Right Touchpad. Applies on save.");
    ImGui::SameLine();
    text_setting("GyroOffButtons", "Comma-separated (e.g. Triangle)");

    section("Controller emulation", s);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("XInput controller emulation");
    help("Show a non-Xbox pad to the game as an Xbox controller.\nOff / Auto (only when the game doesn't read the pad "
         "itself) / Always.\nRestart the game to apply.");
    const char* kXInput[] = {"Off", "Auto", "Always"};
    int xinput = get_int("XInput", 1);
    if (ImGui::Combo("##XInput", &xinput, kXInput, 3)) set("XInput", std::to_string(xinput));
    reset_arrow("XInput", xinput != 1, "1");

    checkbox("Swap A/B and X/Y (Nintendo layout)", "XInputNintendoLabels");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("XInputNintendoLabels: face buttons by label, not position (Nintendo A = Xbox A).\n"
                          "Restart the game to apply.");
    checkbox("Touchpad click also presses View (Back)", "XInputTouchpadBack", 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("XInputTouchpadBack: the DualSense touchpad click acts as the Create/View button.\n"
                          "Restart the game to apply.");

    section("Flick stick", s);
    checkbox("Flick stick with the game's own PlayStation support", "NativeFlickStick");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("NativeFlickStick: flick stick (F10 panel) also in games with their own DualSense/DualShock\n"
                          "support, keeping PlayStation prompts. Restart the game to apply.");

    section("Stick camera", s);
    checkbox("Stick camera (right stick turns the camera itself)", "StickCamera");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("StickCamera: for games whose right-stick turn is too slow even at full deflection.\n"
                          "Turning it on or off needs a restart; the values below apply on save.");
    if (get_int("StickCamera", 0)) {
        slider_int("Speed (yaw, deg/s)", "StickSpeed", 60, 1080, 360);
        slider_int("Speed (pitch, deg/s)", "StickSpeedPitch", 60, 1080, 200);
        slider_int("Deadzone (%)", "StickDeadzone", 0, 50, 10);
        slider_float("Response curve", "StickCurve", 1.0f, 4.0f, 2.0f);
        slider_int("Edge (%)", "StickEdge", 0, 100, 90);
        slider_int("Edge speed (deg/s)", "StickEdgeSpeed", 60, 1080, 720);
        slider_int("Edge ramp (ms)", "StickEdgeRamp", 0, 2000, 400);
        checkbox("Invert Y", "StickInvertY");
        // Free-text list, committed when the box loses focus or Enter is pressed. The default width leaves room
        // for the reset arrow; resetting also clears the edit buffer (via the key resync below).
        static char pass[128] = "";
        static std::string pass_key;
        if (pass_key != "StickCameraPassButtons") {
            pass_key = "StickCameraPassButtons";
            const std::string saved = get(pass_key.c_str());
            std::snprintf(pass, sizeof pass, "%s", saved.c_str());
        }
        const bool enter = ImGui::InputTextWithHint("##pass", "Pass buttons, comma-separated (e.g. RB,LB)", pass,
                                                    sizeof pass, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) set("StickCameraPassButtons", pass);
        if (reset_arrow("StickCameraPassButtons", !get("StickCameraPassButtons").empty(), "")) pass_key.clear();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("StickCameraPassButtons: held +200 ms, they give the right stick back to the game\n"
                              "(weapon wheels). Applies on save.");
    }

    section("Zoom compensation", s);
    ImGui::TextDisabled("Switch \"Adapt to Zoom\" on in the F10 panel's main tab (the ADS tabs inherit it).");
    ImGui::Text("Unzoomed FOV (horizontal degrees)");
    help("ZoomReferenceFOV: the game's field of view when not aiming, in horizontal degrees (as most games'\n"
         "FOV setting). 0 = the game's own value where Gyro4All reads it; manual zoom then counts from 90.\n"
         "Applies on save.");
    slider_float("##ZoomReferenceFOV", "ZoomReferenceFOV", 0.0f, 150.0f, 0.0f, "%.0f");
    ImGui::SameLine();
    ImGui::TextDisabled("0 = auto");
    slider_int("Strength (%)", "ZoomStrength", 0, 200, 100);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ZoomStrength: 100 = a zoom moves the view on screen as fast as unzoomed play; lower keeps\n"
                          "zoomed views faster (75: a 4x scope turns 1.4x faster than at 100); 0 = no effect.\n"
                          "Applies on save.");
    static const char* kSources[] = {"", "Manual"};
    static const char* kSourceLabels[] = {"Auto (the game's FOV where Gyro4All reads it)", "Manual (the levels below)"};
    combo("FOV source##ZoomSource", "ZoomSource", kSources, kSourceLabels, IM_ARRAYSIZE(kSources));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ZoomSource: Auto uses the game's own field of view where the engine reports it, and the\n"
                          "manual levels elsewhere. Manual: always the levels (a game whose reported FOV doesn't\n"
                          "follow its scopes). Applies on save.");
    button_mode_row("zoom_aim", "Aim button",
                    "ZoomAimButton / ZoomAimMode: manual zoom, for games whose field of view Gyro4All can't read.\n"
                    "Held (or toggled) it selects the first zoom level below. Applies on save.",
                    "ZoomAimButton", "ZoomAimMode", s);
    if (!get("ZoomAimButton").empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Next zoom level");
        help("ZoomCycleButton: while aiming, each press goes to the next zoom level, back to the first after the\n"
             "last. Gyro4All can't see the weapon: one with fewer levels than the list gets out of step until\n"
             "the aim button is released or a reset button is pressed. Applies on save.");
        ImGui::SameLine();
        button_combo("ZoomCycleButton", ImGui::GetContentRegionAvail().x * 0.5f);
        ImGui::Text("Zoom levels (horizontal degrees, comma-separated)");
        help("ZoomLevelsFOV: the field of view of each zoom level, the aim first (e.g. 55,30,15).\n"
             "Compared with the unzoomed FOV above. Applies on save.");
        text_setting("ZoomLevelsFOV", "e.g. 55,30,15");
        checkbox("Remember the last zoom level", "ZoomRememberLevel");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ZoomRememberLevel: the next aim starts at the level used last (games that keep each\n"
                              "weapon's zoom) instead of the first. Applies on save.");
        ImGui::Text("Reset buttons (usually the weapon switch)");
        help("ZoomResetButtons: pressing one brings the zoom back to the first level (comma-separated, e.g. Y\n"
             "or Y,Left,Right). Applies on save.");
        text_setting("ZoomResetButtons", "e.g. Y");
        slider_int("Ramp (ms)", "ZoomRampMs", 0, 1000, 150);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ZoomRampMs: the gyro eases to each level's speed over this time, like the game's zoom\n"
                              "animation. Applies on save.");
    }

    section("Sensitivity profiles (optional: other gyro settings while aiming)", s);
    profile_row("ads", "ADS (aiming down sights)", "AdsProfileButton", "AdsProfileMode", s);
    profile_row("alt_ads", "ALT_ADS (second aim)", "AltAdsProfileButton", "AltAdsProfileMode", s);
    profile_row("other", "OTHER", "OtherProfileButton", "OtherProfileMode", s);

    section("Sensor pacing", s);
    checkbox("Play each frame's own sensor time (sensor pacing)", "SensorPacing", 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("SensorPacing: turn by the motion over each frame's own length instead of the reports\n"
                          "that happened to arrive in it (an even turn at a steady hand). Applies on save.");
    if (get_int("SensorPacing", 1)) {
        slider_int("Pacing delay (us, -1 = auto)", "SensorPacingDelayUs", -1, 5000, -1);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("SensorPacingDelayUs: how far behind the frame the motion is played.\n-1 = automatic, "
                              "0 = none. Applies on save.");
    }

    section("Notices", s);
    checkbox("On-screen notices", "Notices", 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Notices=0 keeps Gyro4All's notices in the log only.\nRestart the game to apply.");
    end_sections(s);
}

void expanded(const PanelBox& panel, const ImVec2& at) {
    (void)at;  // the window covers the whole panel; the button sits under it (the × in the header folds it back)
    const float s = panel.scale;
    ScopedScale scale_ui(s);
    // Exactly where GyroLib's panel is: same centred position and size. winmm.dll closes GyroLib's panel while this
    // window is expanded (one panel reads the controller at a time) and reopens it on fold. Font 17*s: one point under
    // GyroLib's 18*s.
    ImGui::SetNextWindowPos(panel.center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(panel.width, panel.height), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x,
                                                    ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).y,
                                                    ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).z, 1.0f));
    if (!ImGui::Begin("Gyro4All", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    // GyroLib's panel is opaque and focused by clicks: keep this window in front of it (the z-order would
    // otherwise bury the window behind the panel until a restart). Not while one of our popups (a combo's
    // dropdown) is open: the popup is its own window, and fronting ours again would bury the dropdown behind
    // this opaque window — the select would "never open".
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PushFont(nullptr, 17.0f * s);
    if (!header("Gyro4All settings", s)) {
        g_expanded = false;
        g_restore_pending = true;  // nav focus back to GyroLib's panel (shared context; see toggle())
        g_event = G4A_SETTINGS_FOLDED;
    } else {
        // BeginChild/EndChild stay paired in both branches: an unbalanced EndChild on the fold frame was ImGui's
        // "Calling End() too many times!" user error, drawn as the thin red border around the window on close.
        if (ImGui::BeginChild("##g4a_body", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar))
            widgets(s);
        ImGui::EndChild();
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
}

}  // namespace

void set_settings(const g4a_settings* settings, gl_context* ctx) {
    g_settings = settings;
    g_ctx = ctx;
}

void toggle() {
    g_expanded = !g_expanded;
    if (g_expanded)
        g_focus_pending = true;  // the next draw() focuses the window for gamepad nav
    else
        g_restore_pending = true;  // nav focus back to GyroLib's panel window, when it is in this context
    g_event = g_expanded ? G4A_SETTINGS_EXPANDED : G4A_SETTINGS_FOLDED;
}

void close() {
    if (!g_expanded) return;
    g_expanded = false;
    g_event = G4A_SETTINGS_CLOSED;
}

bool expanded() { return g_expanded; }

uint32_t take_event() {
    const uint32_t event = g_event;
    g_event = G4A_SETTINGS_NONE;
    return event;
}

// Gamepad-nav focus moves: ours into the unfolded window, and back to GyroLib's panel on fold. Both run inside
// draw() (the owner thread, under g_lock, mid-frame is fine for FocusWindow). "GyroLib###GyroLibPanel" is the name
// gl_panel_draw gives its window; in DX12 games that window lives in gyrolib.dll's own context, FindWindowByName
// finds nothing there and GyroLib keeps its focus by itself.
void focus_pending() {
    if (g_focus_pending) {
        g_focus_pending = false;
        if (ImGuiWindow* w = ImGui::FindWindowByName("Gyro4All"))
            ImGui::FocusWindow(w, ImGuiFocusRequestFlags_RestoreFocusedChild);
    }
    if (g_restore_pending) {
        g_restore_pending = false;
        if (ImGuiWindow* panel = ImGui::FindWindowByName("GyroLib###GyroLibPanel"))
            ImGui::FocusWindow(panel, ImGuiFocusRequestFlags_RestoreFocusedChild);
        else
            ImGui::FocusWindow(nullptr);  // DX12: nothing of ours keeps the nav focus (see the folded button)
    }
}

void draw(float dpi_scale, bool gyrolib_open) {
    if (!g_settings || (!g_expanded && !gyrolib_open)) return;  // the folded button belongs to GyroLib's panel
    g_dpi = dpi_scale;
    const PanelBox panel = panel_box(ImGui::GetMainViewport());
    g_s = panel.scale;  // helpers that draw next to widgets (the reset arrow) use this
    focus_pending();
    const float s = panel.scale;
    const ImVec2 at = button_pos(panel);
    if (!g_expanded) {
        ScopedScale scale_ui(s);
        // Tighter padding than the expanded window: this is just a button. Its height is the 18*s frame plus the
        // padding, ~40*s, centred on the footer's action row. Not navigable: the controller reaches it with L3+R3 (the
        // mouse with a click). Navigable, it kept the nav focus after a fold in DX12 games (GyroLib's panel is in
        // another ImGui context, so focus can't go back to it here): highlighted while the d-pad moved through
        // GyroLib's panel, and Cross/A on any GyroLib row expanded it again.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 6.0f * s));
        ImGui::SetNextWindowPos(at, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##g4a_folded", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav)) {
            // Stay in front of GyroLib's opaque panel after clicks focus it (see expanded()).
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ImGui::PushFont(nullptr, 18.0f * s);
            const float h = ImGui::GetFrameHeight();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            cog(ImGui::GetWindowDrawList(), ImVec2(pos.x + h * 0.5f, pos.y + h * 0.5f), h * 0.40f, 2.0f * s,
                ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + h + 8.0f * s);
            if (ImGui::Button("Gyro4All settings")) {
                g_expanded = true;
                g_focus_pending = true;
                g_event = G4A_SETTINGS_EXPANDED;
            }
            ImGui::SameLine(0, 6.0f * s);
            // The chord hint: L3+R3 named like the connected pad's sticks, smaller than the button and vertically
            // centred on it, a nudge (1% of the button's height) above the exact middle, relative to the button's
            // top edge (SameLine continues at that Y).
            ImGui::PushFont(nullptr, 12.0f * s);
            const char* chord = playstation_pad() ? "(L3+R3)" : "(LS+RS)";
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::CalcTextSize(chord).y) * 0.25f);
            ImGui::TextDisabled("%s", chord);
            ImGui::PopFont();
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    } else {
        expanded(panel, at);
    }
}

}  // namespace g4a::settings_ui
