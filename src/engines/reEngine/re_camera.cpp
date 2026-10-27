#include "re_camera.hpp"

#include "log.hpp"
#include "mem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <numbers>

namespace gyro4all::re {

namespace {

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

Quat operator*(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat axis_angle(double x, double y, double z, double degrees) {
    const double h = degrees * std::numbers::pi / 360;
    return {x * std::sin(h), y * std::sin(h), z * std::sin(h), std::cos(h)};
}

}  // namespace

uintptr_t find_component(const Tdb& tdb, uintptr_t game_object, uint32_t type) {
    const uintptr_t transform = game_object ? mem::rdp(game_object + kGameObjectTransform) : 0;
    uintptr_t c = transform;
    for (int i = 0; c && i < 1024; ++i) {
        if (is_a(tdb, object_type(tdb, c), type)) return c;
        c = mem::rdp(c + kComponentNext);
        if (c == transform) break;
    }
    return 0;
}

uint32_t find_nested_type(const Tdb& tdb, const char* full_name) {
    for (uint32_t i = 1; i < tdb.num_types; ++i)
        if (type_name(tdb, i) == full_name) return i;
    return 0;
}

bool read_rotation(uintptr_t address, double& yaw, double& pitch, double pitch_sign) {
    float q[4];
    if (!mem::rd(address, q)) return false;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double len = x * x + y * y + z * z + w * w;
    if (!(len > 0.9 && len < 1.1)) return false;
    const double fx = 2 * (x * z + w * y), fy = 2 * (y * z - w * x), fz = 1 - 2 * (x * x + y * y);
    yaw = std::atan2(fx, fz) * 180 / std::numbers::pi;
    pitch = pitch_sign * std::asin(std::clamp(fy, -1.0, 1.0)) * 180 / std::numbers::pi;
    return true;
}

bool turn_rotation(uintptr_t address, double pitch, double yaw, double lower, double upper, double pitch_sign) {
    float raw[4];
    double current_yaw = 0, current_pitch = 0;
    if (!mem::rd(address, raw) || !read_rotation(address, current_yaw, current_pitch, pitch_sign)) return false;
    lower = std::clamp(lower, -85.0, 0.0);
    upper = std::clamp(upper, 0.0, 85.0);
    const double target =
        std::clamp(current_pitch + pitch, std::min(lower, current_pitch), std::max(upper, current_pitch));
    Quat q{raw[0], raw[1], raw[2], raw[3]};
    // Yaw about the world's up axis (pre-multiplied), pitch about the camera's own X axis (post-multiplied). A positive
    // angle about +Y turns left and about +X looks down, hence the minus signs (checked on screen in RE7).
    q = axis_angle(0, 1, 0, -yaw) * q * axis_angle(1, 0, 0, -pitch_sign * (target - current_pitch));
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float out[4] = {float(q.x / n), float(q.y / n), float(q.z / n), float(q.w / n)};
    return mem::guarded_store(address, out, sizeof(out));
}

namespace {

// One member field of a watched type: where it is, how many bytes to compare and how to print them.
struct WatchField {
    std::string name, type;
    int32_t offset = 0;
    uint32_t size = 0;
    char kind = 'x';  // f floats, b byte, i int32, r reference, x bytes
};

// Bytes a field of this type takes, 0 if unknown (then up to the next field).
uint32_t known_size(const std::string& t) {
    if (t == "System.Single" || t == "System.Int32" || t == "System.UInt32") return 4;
    if (t == "System.Boolean" || t == "System.Byte" || t == "System.SByte") return 1;
    if (t == "System.Int16" || t == "System.UInt16") return 2;
    if (t == "System.Double" || t == "System.Int64" || t == "System.UInt64") return 8;
    if (t == "via.Quaternion" || t == "via.vec4" || t == "via.Float4") return 16;
    if (t == "via.vec3" || t == "via.Float3") return 12;
    if (t == "via.vec2" || t == "via.Float2") return 8;
    return 0;
}

char kind_of(const std::string& t, uint32_t size) {
    if (t == "System.Single" || t == "via.Quaternion" || t.starts_with("via.vec") || t.starts_with("via.Float")) return 'f';
    if (t == "System.Boolean" || t == "System.Byte") return 'b';
    if (t == "System.Int32" || t == "System.UInt32") return 'i';
    return size == 8 && !t.starts_with("System.") ? 'r' : 'x';
}

// A type's member fields with its parents', sorted by offset (cached per type).
const std::vector<WatchField>& watch_layout(const Tdb& tdb, uint32_t type) {
    static std::map<uint32_t, std::vector<WatchField>> cache;
    auto [it, added] = cache.try_emplace(type);
    if (!added) return it->second;
    std::vector<WatchField>& out = it->second;
    for (uint32_t t = type, depth = 0; t && depth < 16; t = parent_type(tdb, t), ++depth)
        for (const FieldDesc& f : own_fields(tdb, t))
            if (!f.is_static && f.offset >= 0) out.push_back({f.name, type_name(tdb, f.type), f.offset});
    std::sort(out.begin(), out.end(), [](const WatchField& a, const WatchField& b) { return a.offset < b.offset; });
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t gap = i + 1 < out.size() ? uint32_t(out[i + 1].offset - out[i].offset) : 8;
        const uint32_t known = known_size(out[i].type);
        out[i].size = std::min<uint32_t>(known ? known : gap, 16);
        out[i].kind = kind_of(out[i].type, out[i].size);
    }
    return out;
}

std::string short_type(const Tdb& tdb, uint32_t type) {
    const std::string name = type ? type_name(tdb, type) : "none";
    return name.substr(name.find_last_of('.') + 1);
}

std::string format_value(const Tdb& tdb, const WatchField& f, const uint8_t* b) {
    switch (f.kind) {
    case 'f': {
        std::string out;
        for (uint32_t i = 0; i + 4 <= f.size; i += 4) {
            float v;
            std::memcpy(&v, b + i, 4);
            out += std::format("{}{:.3f}", i ? "," : "", v);
        }
        return f.size > 4 ? "(" + out + ")" : out;
    }
    case 'b': return std::to_string(b[0]);
    case 'i': {
        int32_t v;
        std::memcpy(&v, b, 4);
        return std::to_string(v);
    }
    case 'r': {
        uintptr_t p;
        std::memcpy(&p, b, 8);
        const uint32_t t = p ? object_type(tdb, p) : 0;
        return !p ? "null" : t ? short_type(tdb, t) : std::format("0x{:x}", p);
    }
    default: {
        std::string out = "0x";
        for (uint32_t i = f.size; i-- > 0;) out += std::format("{:02x}", b[i]);
        return out;
    }
    }
}

using WatchValues = std::map<std::string, std::pair<const WatchField*, std::string>>;  // label -> field, bytes

// Adds the object's fields to values, and those of the managed objects it references while depth > 0.
void collect(const Tdb& tdb, uintptr_t object, const std::string& prefix, int depth, WatchValues& values) {
    const uint32_t type = object_type(tdb, object);
    const int32_t base = field_base(object);
    if (!type || base < 0) return;
    for (const WatchField& f : watch_layout(tdb, type)) {
        uint8_t bytes[16] = {};
        if (!f.size || !mem::guarded_copy(bytes, object + base + f.offset, f.size)) continue;
        values[prefix + f.name] = {&f, std::string(reinterpret_cast<char*>(bytes), f.size)};
        if (f.kind == 'r' && depth > 0) {
            uintptr_t p;
            std::memcpy(&p, bytes, 8);
            if (p && mem::plausible(p, 16)) collect(tdb, p, prefix + f.name + ".", depth - 1, values);
        }
    }
}

std::string value_text(const Tdb& tdb, const WatchValues::mapped_type& v) {
    return format_value(tdb, *v.first, reinterpret_cast<const uint8_t*>(v.second.data()));
}

}  // namespace

void watch_fields(const Tdb& tdb, int slot, uintptr_t object, uint64_t now_ms) {
    struct State {
        uintptr_t last_object = 0;
        uint64_t next_ms = 0;
        WatchValues last;
    };
    static State states[2];
    static int lines = 0;
    constexpr int kMaxLines = 4000;
    if (slot < 0 || slot >= 2 || !object || lines > kMaxLines) return;
    auto& [last_object, next_ms, last] = states[slot];
    if (object == last_object && now_ms < next_ms) return;
    next_ms = now_ms + 500;
    WatchValues values;
    collect(tdb, object, "", 1, values);
    const std::string type = short_type(tdb, object_type(tdb, object));
    std::string line;
    if (object != last_object) {
        last_object = object;
        log("watch: %s (%s), %zu fields with the objects it references:", type.c_str(),
            mem::module_offset(object).c_str(), values.size());
        for (const auto& [label, v] : values) {
            line += std::format(" {}+0x{:x} {}={}", label, v.first->offset, v.first->type, value_text(tdb, v));
            if (line.size() > 400) {
                log("watch: %s", line.c_str());
                line.clear();
                ++lines;
            }
        }
        if (!line.empty()) log("watch: %s", line.c_str());
    } else {
        for (const auto& [label, v] : values) {
            const auto it = last.find(label);
            if (it == last.end() || it->second.second != v.second) line += std::format(" {}={}", label, value_text(tdb, v));
        }
        if (!line.empty()) log("watch: %s changed:%s", type.c_str(), line.c_str());
    }
    if (!line.empty() && ++lines > kMaxLines) log("watch: %d lines written, CameraWatch stops", kMaxLines);
    last = std::move(values);
}

}  // namespace gyro4all::re
