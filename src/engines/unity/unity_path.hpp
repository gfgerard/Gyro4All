#pragma once
// The member paths of a Unity game's profile ([Unity] in profiles\<exe stem>.ini): which managed members lead from the
// player object to its transforms, limits, conditions and camera. No game access here (unit-tested).
//
//   member.member...                          from the player object ("" = the player itself)
//   Assembly:Namespace.Class::member.member   from a static field or property of a class (no namespace: Assembly:Class::m)
//   !path                                     (conditions only) true when the path's value is false
//
// Each member is a field or a property of the object's class or one of its parents, found by name through Mono's
// reflection. A number ("-89", "75.5") stands for a constant where a value is expected.
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gyro4all::unity {

struct MemberPath {
    std::string assembly;  // static root's assembly name (without .dll); empty: the path starts at the player
    std::string ns, cls;   // static root's namespace ("" = global) and class
    std::vector<std::string> members;
    bool negate = false;   // "!" prefix
    std::optional<double> constant;  // the text was a number

    bool from_player() const { return assembly.empty() && !constant; }
};

namespace path_detail {

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

inline bool identifier(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '<' || c == '>' || c == '`';  // compiler-generated names: <Instance>k__BackingField
        if (!ok) return false;
    }
    return !(s.front() >= '0' && s.front() <= '9');
}

inline bool split_members(std::string_view s, std::vector<std::string>& out, std::string& error) {
    if (s.empty()) return true;
    for (;;) {
        const size_t dot = s.find('.');
        const std::string_view part = s.substr(0, dot);
        if (!identifier(part)) {
            error = "'" + std::string(part) + "' is not a member name";
            return false;
        }
        out.emplace_back(part);
        if (dot == std::string_view::npos) return true;
        s.remove_prefix(dot + 1);
    }
}

}  // namespace path_detail

// Parses one path; nullopt with error set when it isn't one. allow_negate: "!" is accepted (conditions).
inline std::optional<MemberPath> parse_path(std::string_view text, std::string& error, bool allow_negate = false) {
    using namespace path_detail;
    MemberPath p;
    std::string_view s = trim(text);
    if (!s.empty() && s.front() == '!') {
        if (!allow_negate) {
            error = "'!' only goes before a condition";
            return std::nullopt;
        }
        p.negate = true;
        s = trim(s.substr(1));
    }
    if (!s.empty() && ((s.front() >= '0' && s.front() <= '9') || s.front() == '-' || s.front() == '+')) {
        const std::string number(s);
        char* end = nullptr;
        const double v = std::strtod(number.c_str(), &end);
        if (end != number.c_str() + number.size()) {
            error = "'" + number + "' is not a number";
            return std::nullopt;
        }
        p.constant = v;
        return p;
    }
    if (s == ".") s = {};
    const size_t colon = s.find(':');
    if (colon != std::string_view::npos && s.substr(colon, 2) != "::") {
        const size_t member_sep = s.find("::", colon + 1);
        if (member_sep == std::string_view::npos) {
            error = "a static path needs '::' before its member (Assembly:Class::Member)";
            return std::nullopt;
        }
        p.assembly = std::string(trim(s.substr(0, colon)));
        const std::string_view type = trim(s.substr(colon + 1, member_sep - colon - 1));
        if (p.assembly.empty() || type.empty()) {
            error = "a static path needs an assembly and a class (Assembly:Class::Member)";
            return std::nullopt;
        }
        const size_t last_dot = type.rfind('.');
        p.cls = std::string(last_dot == std::string_view::npos ? type : type.substr(last_dot + 1));
        if (last_dot != std::string_view::npos) p.ns = std::string(type.substr(0, last_dot));
        if (!identifier(p.cls)) {
            error = "'" + p.cls + "' is not a class name";
            return std::nullopt;
        }
        s = trim(s.substr(member_sep + 2));
        if (s.empty()) {
            error = "a static path needs a member after '::'";
            return std::nullopt;
        }
    } else if (colon != std::string_view::npos) {
        error = "'::' needs an assembly and a class before it (Assembly:Class::Member)";
        return std::nullopt;
    }
    if (!split_members(s, p.members, error)) return std::nullopt;
    return p;
}

// A comma-separated list of paths (conditions); empty entries are skipped. False with error at the first bad one.
inline bool parse_path_list(std::string_view text, std::vector<MemberPath>& out, std::string& error,
                            bool allow_negate = true) {
    out.clear();
    for (;;) {
        const size_t comma = text.find(',');
        const std::string_view part = path_detail::trim(text.substr(0, comma));
        if (!part.empty()) {
            auto p = parse_path(part, error, allow_negate);
            if (!p) return false;
            out.push_back(std::move(*p));
        }
        if (comma == std::string_view::npos) return true;
        text.remove_prefix(comma + 1);
    }
}

// The path as the profile would write it (for the log).
inline std::string describe(const MemberPath& p) {
    if (p.constant) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", *p.constant);
        return buf;
    }
    std::string s = p.negate ? "!" : "";
    if (!p.assembly.empty()) s += p.assembly + ":" + (p.ns.empty() ? "" : p.ns + ".") + p.cls + "::";
    for (size_t i = 0; i < p.members.size(); ++i) s += (i ? "." : "") + p.members[i];
    if (p.assembly.empty() && p.members.empty()) s += "(the player)";
    return s;
}

}  // namespace gyro4all::unity
