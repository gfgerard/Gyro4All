#include "perf.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace gyro4all::perf {
namespace {

std::mutex g_mutex;
std::vector<float> g_samples[Count];  // microseconds, current window
double g_freq_us = 0;

const char* kNames[Count] = {"idle", "active", "present"};

}  // namespace

int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void record(Counter c, int64_t start) {
    const int64_t end = now();
    std::lock_guard lock(g_mutex);
    if (g_freq_us == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq_us = double(f.QuadPart) / 1e6;
    }
    auto& v = g_samples[c];
    if (v.size() < 200000) v.push_back(float(double(end - start) / g_freq_us));
}

void report() {
    std::vector<float> windows[Count];
    {
        std::lock_guard lock(g_mutex);
        for (int c = 0; c < Count; ++c) windows[c].swap(g_samples[c]);
    }
    char line[768];
    int n = 0;
    for (int c = 0; c < Count; ++c) {
        auto& v = windows[c];
        if (v.empty()) {
            n += std::snprintf(line + n, sizeof(line) - n, " %s: -", kNames[c]);
            continue;
        }
        double sum = 0;
        for (float x : v) sum += x;
        std::sort(v.begin(), v.end());
        const float p99 = v[std::min(v.size() - 1, size_t(v.size() * 0.99))];
        n += std::snprintf(line + n, sizeof(line) - n, " %s: n=%zu avg=%.1fus p99=%.1fus max=%.1fus", kNames[c],
                           v.size(), sum / v.size(), p99, v.back());
    }
    log("perf:%s", line);
}

}  // namespace gyro4all::perf
