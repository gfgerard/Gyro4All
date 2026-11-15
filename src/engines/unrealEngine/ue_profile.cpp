#include "ue_profile.hpp"

#include "log.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace gyro4all::unreal {
namespace {

std::string ini(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(section, key, fallback, buffer, 2048, path.c_str());
    return narrow(buffer);
}

std::wstring widen(const std::string& text) {
    std::wstring out(text.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    out.resize(n > 0 ? size_t(n) : 0);
    return out;
}

void load_view_extras(View& v, const std::wstring& path, const wchar_t* section) {
    v.object = split(ini(path, section, L"Object"), '.');
    for (const std::string& o : split(ini(path, section, L"Offsets"), ','))
        v.offsets.push_back(int32_t(std::strtol(o.c_str(), nullptr, 0)));
    v.rotator_size = GetPrivateProfileIntW(section, L"RotatorSize", 24, path.c_str());
    v.world_rotation = GetPrivateProfileIntW(section, L"WorldRotation", 0, path.c_str()) != 0;
    v.matrix = GetPrivateProfileIntW(section, L"Matrix", 0, path.c_str()) != 0;
    const std::string active = ini(path, section, L"ActiveList"), output = ini(path, section, L"OutputMatrix");
    if (!active.empty()) v.active_list = int32_t(std::strtol(active.c_str(), nullptr, 0));
    v.active_skip = split(ini(path, section, L"ActiveListSkip"), '|');
    if (!output.empty()) v.output_matrix = int32_t(std::strtol(output.c_str(), nullptr, 0));
    v.pitch_view = GetPrivateProfileIntW(section, L"PitchView", 0, path.c_str());
    v.pitch_object = split(ini(path, section, L"PitchObject"), '.');
    v.pitch_property = ini(path, section, L"PitchProperty");
    for (const std::string& o : split(ini(path, section, L"PitchOffsets"), ','))
        v.pitch_offsets.push_back(int32_t(std::strtol(o.c_str(), nullptr, 0)));
    v.pitch_rotator_size = GetPrivateProfileIntW(section, L"PitchRotatorSize", v.rotator_size, path.c_str());
    if (!v.pitch_object.empty() && !v.has_pitch_rotation())
        log("profile: [%ls] PitchObject= ignored: needs PitchProperty= or PitchOffsets=", section);
    v.watch = split(ini(path, section, L"Watch"), ',');
    v.watched_values.assign(v.watch.size(), std::string{});
    v.dump = GetPrivateProfileIntW(section, L"Dump", 0, path.c_str()) != 0;
    v.recenter = GetPrivateProfileIntW(section, L"Recenter", 1, path.c_str()) != 0;
    v.recenter_pitch = std::atof(ini(path, section, L"RecenterPitch", L"0").c_str());
    // The unzoomed FOV for zoom compensation: the view's own ZoomReferenceFOV= wins; else the generic [Gyro4All]
    // ZoomReferenceFOV= (any engine's fallback); 0 reads PlayerCameraManager.DefaultFOV.
    const std::string own_fov = ini(path, section, L"ZoomReferenceFOV");
    v.zoom_reference_fov =
        std::atof((own_fov.empty() ? ini(path, settings::kSection, L"ZoomReferenceFOV") : own_fov).c_str());
    for (const std::string& item : split(ini(path, section, L"OnGyro"), ',')) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::vector<std::string> object = split(item.substr(0, colon), '.');
        if (object.empty()) continue;
        View::OnGyro w;
        w.property = object.back();
        object.pop_back();
        w.object = std::move(object);
        w.value = std::atof(item.c_str() + colon + 1);
        v.on_gyro.push_back(std::move(w));
    }
}

// Views= from [Gyro4All], else [GyroUE] (a profile from GyroUE).
std::vector<std::string> view_keys(const std::wstring& path) {
    std::wstring list;
    return settings::read(path, L"Views", list) ? split(narrow(list), ',') : std::vector<std::string>{};
}

std::string join(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : std::string(1, sep)) + p;
    return out;
}

}  // namespace

bool has_views(const std::wstring& path) {
    wchar_t buffer[64];
    std::wstring list;
    return GetPrivateProfileSectionW(L"View.Default", buffer, 64, path.c_str()) > 0 || settings::read(path, L"Views", list);
}

std::vector<View> load_views(const std::wstring& path) {
    View def;
    def.label = ini(path, L"View.Default", L"Label", L"Exploration");
    def.property = ini(path, L"View.Default", L"Property");
    load_view_extras(def, path, L"View.Default");
    std::vector<View> views = {def};
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log("profile: %s not found, using the generic ControlRotation view only", narrow(path).c_str());
        return views;
    }
    for (const std::string& key : view_keys(path)) {
        const std::wstring section = L"View." + widen(key);
        View v;
        v.key = key;
        v.id = GetPrivateProfileIntW(section.c_str(), L"Id", 0, path.c_str());
        v.label = ini(path, section.c_str(), L"Label", widen(key).c_str());
        v.view_target_class = ini(path, section.c_str(), L"ViewTargetClass");
        v.match_camera = _stricmp(ini(path, section.c_str(), L"Match").c_str(), "Camera") == 0;
        v.property = ini(path, section.c_str(), L"Property");
        v.parent = GetPrivateProfileIntW(section.c_str(), L"Parent", 1, path.c_str());
        load_view_extras(v, path, section.c_str());
        const bool duplicate_id = std::any_of(views.begin(), views.end(), [&](const View& o) { return o.id == v.id; });
        if (v.id <= 1 || duplicate_id || (v.view_target_class.empty() && !v.match_camera && v.active_list < 0) ||
            (v.property.empty() && v.offsets.empty())) {
            log("profile: view %s ignored (needs a unique Id > 1, ViewTargetClass, Match=Camera or ActiveList, and "
                "Property or Offsets)", v.key.c_str());
            continue;
        }
        views.push_back(std::move(v));
    }
    log("profile: %s, %zu view(s)", narrow(path).c_str(), views.size());
    for (const View& v : views) {
        std::string where;
        for (int32_t off : v.offsets) where += (where.empty() ? "raw +0x" : ",0x") + std::format("{:X}", off);
        if (where.empty()) where = v.property.empty() ? "ControlRotation" : v.property;
        else if (v.matrix) where += " (rotation matrices)";
        if (v.output_matrix >= 0) where += std::format(" (output +0x{:X})", v.output_matrix);
        if (v.active_list >= 0) where += std::format(" (active while first in the list at +0x{:X})", v.active_list);
        if (v.world_rotation) where += v.offsets.empty() ? " (WorldRotation)" : " (WorldRotation ignored: needs Property=)";
        if (v.has_pitch_rotation()) {
            std::string pitch = v.pitch_object.empty() ? std::string("(same object)") : join(v.pitch_object, '.');
            if (!v.pitch_property.empty()) pitch += "." + v.pitch_property;
            for (size_t i = 0; i < v.pitch_offsets.size(); ++i)
                pitch += std::format("{}0x{:X}", i ? "," : " raw +", v.pitch_offsets[i]);
            where += " (yaw only, pitch in " + pitch + ")";
        } else if (v.pitch_view) {
            where += std::format(" (yaw only, pitch in view {})", v.pitch_view);
        }
        const std::string target = !v.view_target_class.empty() ? v.view_target_class
                                   : v.match_camera || v.active_list >= 0 ? "(any)"
                                                                            : "(pawn)";
        const std::string object = join(v.object, '.');
        log("  view %u '%s' target=%s%s object=%s rotation=%s parent=%u", v.id, v.label.c_str(), target.c_str(),
            v.match_camera ? " match=camera" : "", object.empty() ? "(default)" : object.c_str(), where.c_str(), v.parent);
    }
    for (const View& v : views)
        if (!v.on_gyro.empty() || !v.watch.empty() || v.dump)
            log("  view %u extras: %zu OnGyro write(s), %zu watch(es), dump=%d", v.id, v.on_gyro.size(), v.watch.size(), v.dump);
    return views;
}

bool same_rotation(const View& view, const LearnedView& learned) {
    // The default view's empty Object/Property is PC.ControlRotation; another view's empty Object is its view target.
    std::string object = join(view.object, '.');
    if (object.empty()) object = view.id == 1 ? "PC" : "ViewTarget";
    const std::string property = view.property.empty() && view.offsets.empty() ? "ControlRotation" : view.property;
    if (object != learned.object) return false;
    if (!learned.property.empty()) return property == learned.property && view.offsets.empty();
    return view.offsets == learned.offsets;
}

AddResult add_view(const std::wstring& path, const std::string& exe_stem, const LearnedView& learned,
                   const std::vector<uint32_t>& reserved, uint32_t& id, std::string& key) {
    id = 0;
    key.clear();
    if (learned.object.empty() || (learned.property.empty() && learned.offsets.empty())) return AddResult::Failed;
    const bool created = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (!created)
        for (const View& v : load_views(path))
            if (same_rotation(v, learned)) {
                id = v.id;
                key = v.key;
                return AddResult::Duplicate;
            }
    // Ids in use: every listed view (valid or not) and the reserved ones.
    std::vector<uint32_t> used = reserved;
    used.push_back(1);
    const std::vector<std::string> keys = created ? std::vector<std::string>{} : view_keys(path);
    for (const std::string& k : keys)
        used.push_back(GetPrivateProfileIntW((L"View." + widen(k)).c_str(), L"Id", 0, path.c_str()));
    id = 2;
    while (std::find(used.begin(), used.end(), id) != used.end()) ++id;
    key = "Auto" + std::to_string(id);
    while (std::find(keys.begin(), keys.end(), key) != keys.end()) key += "b";

    std::string text = created ? "; Gyro4All profile for " + exe_stem + ", written by Gyro4All.\r\n" : std::string();
    text += "\r\n";
    for (const std::string& line : split(learned.note, '\n')) text += "; " + line + "\r\n";
    text += std::format("[View.{}]\r\nId={}\r\nLabel={}\r\nMatch=Camera\r\nParent=1\r\nObject={}\r\n", key, id,
                        learned.label.empty() ? key : learned.label, learned.object);
    if (!learned.property.empty()) {
        text += "Property=" + learned.property + "\r\n";
        if (learned.world_rotation) text += "WorldRotation=1\r\n";
    } else {
        std::string offsets;
        for (int32_t off : learned.offsets) offsets += (offsets.empty() ? "" : ",") + std::format("0x{:X}", off);
        text += "Offsets=" + offsets + "\r\nRotatorSize=" + std::to_string(learned.rotator_size) + "\r\n";
    }
    if (!learned.pitch_object.empty()) text += pitch_keys_text(learned, "\r\n");
    else if (learned.pitch_view) text += "PitchView=" + std::to_string(learned.pitch_view) + "\r\n";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file) return AddResult::Failed;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    if (!written) return AddResult::Failed;
    std::vector<std::string> list = keys;
    list.push_back(key);
    // Into [Gyro4All], which wins over a [GyroUE] Views= of a profile from GyroUE (the list keeps its views).
    if (!WritePrivateProfileStringW(settings::kSection, L"Views", widen(join(list, ',')).c_str(), path.c_str()))
        return AddResult::Failed;
    return AddResult::Added;
}

std::string pitch_keys_text(const LearnedView& learned, const char* eol) {
    if (learned.pitch_object.empty() || (learned.pitch_property.empty() && learned.pitch_offsets.empty())) return {};
    std::string text = "PitchObject=" + learned.pitch_object + eol;
    if (!learned.pitch_property.empty()) return text + "PitchProperty=" + learned.pitch_property + eol;
    std::string offsets;
    for (int32_t off : learned.pitch_offsets) offsets += (offsets.empty() ? "" : ",") + std::format("0x{:X}", off);
    return text + "PitchOffsets=" + offsets + eol + "PitchRotatorSize=" + std::to_string(learned.pitch_rotator_size) + eol;
}

bool set_pitch_rotation(const std::wstring& path, const std::string& key, const LearnedView& pitch) {
    if (pitch_keys_text(pitch, "\n").empty()) return false;
    const std::wstring section = L"View." + widen(key);
    const wchar_t* s = section.c_str();
    const wchar_t* p = path.c_str();
    std::string offsets;
    for (int32_t off : pitch.pitch_offsets) offsets += (offsets.empty() ? "" : ",") + std::format("0x{:X}", off);
    const bool raw = pitch.pitch_property.empty();
    // A Default section written for the first time gets its label, so the panel's tab keeps its usual name.
    if (key == "Default" && ini(path, s, L"Label").empty() && !WritePrivateProfileStringW(s, L"Label", L"Exploration", p))
        return false;
    return WritePrivateProfileStringW(s, L"PitchObject", widen(pitch.pitch_object).c_str(), p) &&
           WritePrivateProfileStringW(s, L"PitchProperty", raw ? nullptr : widen(pitch.pitch_property).c_str(), p) &&
           WritePrivateProfileStringW(s, L"PitchOffsets", raw ? widen(offsets).c_str() : nullptr, p) &&
           WritePrivateProfileStringW(s, L"PitchRotatorSize", raw ? std::to_wstring(pitch.pitch_rotator_size).c_str() : nullptr, p) &&
           WritePrivateProfileStringW(s, L"PitchView", nullptr, p);
}

bool PauseCondition::holds(double v) const {
    switch (op) {
        case Op::NonZero: return v != 0;
        case Op::Eq: return v == value;
        case Op::Ne: return v != value;
        case Op::Lt: return v < value;
        case Op::Le: return v <= value;
        case Op::Gt: return v > value;
        case Op::Ge: return v >= value;
    }
    return false;
}

bool parse_pause_condition(const std::string& text, PauseCondition& out, std::string& error) {
    const auto trim = [](std::string s) {
        const auto blank = [](char c) { return c == ' ' || c == '\t'; };
        while (!s.empty() && blank(s.front())) s.erase(s.begin());
        while (!s.empty() && blank(s.back())) s.pop_back();
        return s;
    };
    out = PauseCondition{};
    out.text = trim(text);
    if (out.text.empty()) return error = "empty", false;
    // The operator: the first of < > = ! in the text; two characters when followed by '='.
    const size_t at = out.text.find_first_of("<>=!");
    const std::string path = trim(out.text.substr(0, at));
    if (at != std::string::npos) {
        const bool two = at + 1 < out.text.size() && out.text[at + 1] == '=';
        const std::string op = out.text.substr(at, two ? 2 : 1);
        if (op == "==") out.op = PauseCondition::Op::Eq;
        else if (op == "!=") out.op = PauseCondition::Op::Ne;
        else if (op == "<") out.op = PauseCondition::Op::Lt;
        else if (op == "<=") out.op = PauseCondition::Op::Le;
        else if (op == ">") out.op = PauseCondition::Op::Gt;
        else if (op == ">=") out.op = PauseCondition::Op::Ge;
        else return error = "unknown operator '" + op + "' (== != < <= > >=)", false;
        const std::string value = trim(out.text.substr(at + op.size()));
        std::string lower = value;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lower.empty()) return error = "no value after the operator", false;
        if (lower == "true") out.value = 1;
        else if (lower == "false") out.value = 0;
        else {
            char* end = nullptr;
            out.value = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(out.value))
                return error = "the value '" + value + "' isn't a number, true or false", false;
        }
    }
    for (const unsigned char c : path)
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '@')
            return error = std::string("the path has a character it can't have ('") + char(c) + "')", false;
    if (path.empty() || path.front() == '.' || path.back() == '.' || path.find("..") != std::string::npos)
        return error = "malformed path", false;
    std::vector<std::string> parts = split(path, '.');
    if (parts.size() < 2) return error = "needs a root and a property, e.g. Pawn.bIsInCutscene", false;
    const std::string& root = parts.front();
    if (root != "PC" && root != "Pawn" && root != "Camera" && root != "ViewTarget")
        return error = "unknown root '" + root + "' (PC, Pawn, Camera or ViewTarget)", false;
    if (parts.back().front() == '@') return error = "the last step must be a property, not a class", false;
    out.property = parts.back();
    parts.pop_back();
    out.object = std::move(parts);
    return true;
}

std::vector<PauseCondition> load_pause_conditions(const std::wstring& path) {
    std::vector<PauseCondition> out;
    std::wstring list;
    if (!settings::read(path, L"PauseConditions", list)) return out;
    for (const std::string& item : split(narrow(list), ',')) {
        PauseCondition c;
        std::string error;
        if (parse_pause_condition(item, c, error)) {
            log("profile: pause condition '%s'", c.text.c_str());
            out.push_back(std::move(c));
        } else {
            log("profile: pause condition '%s' ignored: %s", item.c_str(), error.c_str());
        }
    }
    return out;
}

}  // namespace gyro4all::unreal
