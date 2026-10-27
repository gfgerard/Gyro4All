#include "engine.hpp"

#include "log.hpp"
#include "settings.hpp"

#include <cstring>

namespace gyro4all {

Engine& select_engine(const std::wstring& exe_path) {
    const std::span<Engine* const> engines = all_engines();
    const std::string forced = settings::get_string(L"Engine");
    if (!forced.empty()) {
        for (Engine* e : engines)
            if (_stricmp(e->name(), forced.c_str()) == 0) {
                log("engine: %s (Engine=%s)", e->name(), forced.c_str());
                return *e;
            }
        log("engine: Engine=%s is not a known engine, detecting", forced.c_str());
    }
    std::string tried;
    for (Engine* e : engines) {
        if (e->detect(exe_path)) {
            log("engine: %s (detected)", e->name());
            return *e;
        }
        tried += std::string(tried.empty() ? "" : ", ") + e->name();
    }
    log("engine: none detected (%s), using %s", tried.c_str(), engines.front()->name());
    return *engines.front();
}

}  // namespace gyro4all
