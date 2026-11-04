// Every engine module linked into the DLL, in detection order, and the fallback when none detects the game. Adding an engine: its engines/<engine>/ module, add_subdirectory and link in src/CMakeLists.txt,
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
#include "glacier.hpp"
#include "halo.hpp"
#include "northlight.hpp"
#include "asura.hpp"
#include "rage.hpp"
#include "red.hpp"

namespace gyro4all {

std::span<Engine* const> all_engines() {
    static Engine* const engines[] = {&halo::engine(), &unreal::engine(), &unreal3::engine(), &re::engine(), &fb::engine(), &idtech::engine(), &source::engine(), &foura::engine(), &swarm::engine(), &cryengine::engine(), &glacier::engine(), &northlight::engine(), &asura::engine(), &rage::engine(), &red::engine()};
    return engines;
}

// Unreal: most games; Halo goes first only because MCC's shell is an Unreal exe.
Engine& fallback_engine() { return unreal::engine(); }

}  // namespace gyro4all
