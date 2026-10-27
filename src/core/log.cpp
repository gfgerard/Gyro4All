#include "log.hpp"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace gyro4all {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static FILE* g_probe_file = nullptr;

static void write_line(FILE* file, const char* fmt, va_list args) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(file, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::vfprintf(file, fmt, args);
    std::fputc('\n', file);
    std::fflush(file);
}

void log_open(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (!g_file) g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);  // others may read it while the game runs
}

void log_close() {
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void log(const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    va_list args;
    va_start(args, fmt);
    write_line(g_file, fmt, args);
    va_end(args);
}

bool probe_log_open(const std::wstring& path, const std::string& header) {
    std::lock_guard lock(g_mutex);
    if (!g_probe_file) {
        g_probe_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
        if (g_probe_file) {
            std::fputs(header.c_str(), g_probe_file);
            std::fflush(g_probe_file);
        }
    }
    return g_probe_file != nullptr;
}

void probe_log(const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    FILE* file = g_probe_file ? g_probe_file : g_file;  // falls back to Gyro4All.log if the probe file failed
    if (!file) return;
    va_list args;
    va_start(args, fmt);
    write_line(file, fmt, args);
    va_end(args);
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace gyro4all
