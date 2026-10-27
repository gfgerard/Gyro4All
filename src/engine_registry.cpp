// Every engine module linked into the DLL, in detection order (the first one is also the fallback when none
// detects the game). Adding an engine: its engines/<engine>/ module, add_subdirectory and link in src/CMakeLists.txt,
// and one line here.
#include "engine.hpp"
#include "foura.hpp"
#include "fb.hpp"
#include "idtech.hpp"
#include "re.hpp"
#include "source.hpp"
#include "swarm.hpp"
#include "unreal.hpp"
#include "unreal3.hpp"
#include "cryengine.hpp"

namespace gyro4all {

std::span<Engine* const> all_engines() {
    static Engine* const engines[] = {&unreal::engine(), &unreal3::engine(), &re::engine(), &fb::engine(), &idtech::engine(), &source::engine(), &foura::engine(), &swarm::engine(), &cryengine::engine()};
    return engines;
}

}  // namespace gyro4all
