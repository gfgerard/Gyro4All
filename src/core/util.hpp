#pragma once
// Small helpers shared by the core and the engine modules.
#include <cmath>
#include <string>
#include <vector>

namespace gyro4all {

// Items of a separated list, trimmed of spaces and tabs (a hand-edited "Views=Clamp <tab>" must still name
// [View.Clamp]); empty items are dropped.
inline std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(sep, start);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(start, end - start);
        auto blank = [](char c) { return c == ' ' || c == '\t'; };
        while (!item.empty() && blank(item.front())) item.erase(item.begin());
        while (!item.empty() && blank(item.back())) item.pop_back();
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}

// An angle in degrees, wrapped to [-180, 180].
inline double wrap180(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180) a -= 360;
    if (a < -180) a += 360;
    return a;
}

}  // namespace gyro4all
