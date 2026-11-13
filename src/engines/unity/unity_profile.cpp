#include "unity_profile.hpp"

#include "log.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace gyro4all::unity {
namespace {

std::string setting(const std::wstring& path, const wchar_t* key) {
    wchar_t value[512];
    GetPrivateProfileStringW(L"Unity", key, L"", value, DWORD(std::size(value)), path.c_str());
    return narrow(value);
}

void per_degree(const std::wstring& path, const wchar_t* key, double& out) {
    const std::string text = setting(path, key);
    if (text.empty()) return;
    const double value = std::atof(text.c_str());
    if (std::isfinite(value) && value != 0) out = value;
    else log("unity: profile: %s=%s ignored", narrow(key).c_str(), text.c_str());
}

std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// A path's part as ManagedPath reads it: a name, "name[Class]" or "method(@)"; no spaces or separators inside.
bool member_name(const std::string& part) {
    if (part.empty()) return false;
    for (const char c : part)
        if (std::isspace(static_cast<unsigned char>(c)) || c == '!' || c == ',') return false;
    return true;
}

}  // namespace

bool parse_look(const std::string& text, std::vector<LookCondition>& out) {
    out.clear();
    for (const std::string& raw : split(text, ',')) {
        std::string item = trim(raw);
        if (item.empty()) continue;
        LookCondition c;
        if (item[0] == '!') {
            c.negate = true;
            item = trim(item.substr(1));
        }
        c.path = split(item, '.');
        if (c.path.empty() || c.path.size() != size_t(std::count(item.begin(), item.end(), '.') + 1) ||
            !std::all_of(c.path.begin(), c.path.end(), member_name)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

UnityProfile load_unity_profile(const std::wstring& path) {
    UnityProfile profile;
    profile.object_type = setting(path, L"ObjectType");
    profile.yaw_object_type = setting(path, L"YawObjectType");
    profile.object_name = setting(path, L"ObjectName");
    profile.yaw_object_name = setting(path, L"YawObjectName");
    profile.camera_field = setting(path, L"CameraField");
    profile.path = split(setting(path, L"PitchYawPath"), '.');
    profile.pitch_path = split(setting(path, L"PitchPath"), '.');
    profile.yaw_path = split(setting(path, L"YawPath"), '.');
    profile.pitch_object = split(setting(path, L"PitchObject"), '.');
    profile.yaw_object = split(setting(path, L"YawObject"), '.');
    const std::string look = setting(path, L"Look");
    if (!parse_look(look, profile.look)) log("unity: profile: Look=%s ignored (a malformed condition)", look.c_str());
    const std::string zoom = trim(setting(path, L"ZoomReference"));
    if (!zoom.empty()) {
        char* end = nullptr;
        const double value = std::strtod(zoom.c_str(), &end);
        if (end && *end == '\0') {
            if (value > 1 && value < 179) profile.zoom_reference_value = value;
            else log("unity: profile: ZoomReference=%s ignored", zoom.c_str());
        } else {
            profile.zoom_reference = split(zoom, '.');
        }
    }
    const std::string axis = trim(setting(path, L"ZoomReferenceAxis"));
    profile.zoom_axis = parse_fov_axis(axis);
    if (!axis.empty() && profile.zoom_axis == FovAxis::Auto && axis != "Auto" && axis != "auto")
        log("unity: profile: ZoomReferenceAxis=%s not Vertical or Horizontal: told from the camera", axis.c_str());
    per_degree(path, L"PitchPerDegree", profile.pitch_per_degree);
    per_degree(path, L"YawPerDegree", profile.yaw_per_degree);
    const std::string limit = setting(path, L"PitchLimit");
    if (!limit.empty()) {
        const double value = std::atof(limit.c_str());
        if (value >= 10 && value <= 89.9) profile.pitch_limit = value;
    }
    const int parent = std::atoi(setting(path, L"YawParent").c_str());
    if (parent >= 1 && parent <= 16) profile.yaw_parent = parent;
    const std::string pitch_transform = setting(path, L"PitchTransform");
    profile.pitch_transform = pitch_transform == "1" || pitch_transform == "true";
    const std::string yaw_from_camera = setting(path, L"YawFromCamera");
    profile.yaw_from_camera = yaw_from_camera == "1" || yaw_from_camera == "true";
    profile.auto_found = setting(path, L"Source") == "auto";
    const std::string probe = setting(path, L"Probe");
    profile.probe = probe == "1" || probe == "true";
    return profile;
}

}  // namespace gyro4all::unity
