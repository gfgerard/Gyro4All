#include "ue_profile.hpp"

#include "log.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
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
    v.watch = split(ini(path, section, L"Watch"), ',');
    v.watched_values.assign(v.watch.size(), std::string{});
    v.dump = GetPrivateProfileIntW(section, L"Dump", 0, path.c_str()) != 0;
    v.recenter = GetPrivateProfileIntW(section, L"Recenter", 1, path.c_str()) != 0;
    v.recenter_pitch = std::atof(ini(path, section, L"RecenterPitch", L"0").c_str());
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
        if (v.pitch_view) where += std::format(" (yaw only, pitch in view {})", v.pitch_view);
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
    if (learned.pitch_view) text += "PitchView=" + std::to_string(learned.pitch_view) + "\r\n";
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

}  // namespace gyro4all::unreal
