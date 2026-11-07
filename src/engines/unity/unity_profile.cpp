#include "unity_profile.hpp"

#include "log.hpp"
#include "util.hpp"

#include <windows.h>

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

}  // namespace

UnityProfile load_unity_profile(const std::wstring& path) {
    UnityProfile profile;
    profile.object_type = setting(path, L"ObjectType");
    profile.yaw_object_type = setting(path, L"YawObjectType");
    profile.camera_field = setting(path, L"CameraField");
    profile.path = split(setting(path, L"PitchYawPath"), '.');
    profile.pitch_path = split(setting(path, L"PitchPath"), '.');
    profile.yaw_path = split(setting(path, L"YawPath"), '.');
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
    profile.auto_found = setting(path, L"Source") == "auto";
    const std::string probe = setting(path, L"Probe");
    profile.probe = probe == "1" || probe == "true";
    return profile;
}

}  // namespace gyro4all::unity
