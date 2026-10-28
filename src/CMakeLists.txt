cmake_minimum_required(VERSION 3.24)
project(Gyro4All VERSION 1.8.3 LANGUAGES C CXX)  # the one place the Gyro4All version lives (DLL log line + release zip name)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one less DLL to ship

# GyroLib SDK (pinned in tools/gyrolib-sdk.sh), passed with -DCMAKE_PREFIX_PATH by tools/build-gyro4all.sh
find_package(GyroLib 1.2 CONFIG REQUIRED COMPONENTS SDL Panel)

# MinHook (BSD-2), pinned in third_party/: inline hooks for protected exes whose import table is a stub.
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook-1.3.4")
add_library(minhook STATIC "${MINHOOK_DIR}/src/buffer.c" "${MINHOOK_DIR}/src/hook.c" "${MINHOOK_DIR}/src/trampoline.c"
            "${MINHOOK_DIR}/src/hde/hde64.c")
target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")

# Build settings shared by the core and every engine module.
add_library(gyro_options INTERFACE)
target_compile_definitions(gyro_options INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                           "GYRO4ALL_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(gyro_options INTERFACE /W4 /permissive- /utf-8 /Zi)
target_link_libraries(gyro_options INTERFACE GyroLib::gyrolib_sdl minhook)

# core/: engine-neutral (loader, hooks, GyroLib wiring, XInput, native pad, overlay, notices, the per-frame host).
# engines/<engine>/: one module per game engine, behind core/engine.hpp. Object libraries: every object is linked.
add_subdirectory(core)
add_subdirectory(engines/unrealEngine)
add_subdirectory(engines/reEngine)
add_subdirectory(engines/frostbite)
add_subdirectory(engines/idTech)
add_subdirectory(engines/source)
add_subdirectory(engines/unrealEngine3)
add_subdirectory(engines/4aEngine)
add_subdirectory(engines/swarm)
add_subdirectory(engines/cryEngine)
# panel/: gyro4all.dll, the settings panel for non-DX12 games (dynamic CRT, loaded by winmm.dll only when needed).
add_subdirectory(panel)

# Version resource (file properties: description, product name, version) from project() above.
configure_file(core/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_library(Gyro4All SHARED engine_registry.cpp core/proxy.def "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
set_target_properties(Gyro4All PROPERTIES OUTPUT_NAME winmm)
target_link_libraries(Gyro4All PRIVATE gyro_options gyro_core gyro_unreal gyro_re gyro_frostbite gyro_idtech gyro_source gyro_unreal3 gyro_foura gyro_swarm gyro_cryengine delayimp dxguid)
# gyrolib.dll lives in the Gyro4All subfolder and is loaded by full path before first use.
target_link_options(Gyro4All PRIVATE /DELAYLOAD:gyrolib.dll)
# Symbols for crash dumps (build/Gyro4All/Release/winmm.pdb; never shipped). /OPT keeps the release layout.
# /PDBALTPATH stores only the PDB file name in the DLL, not the build folder.
target_link_options(Gyro4All PRIVATE /DEBUG /PDBALTPATH:%_PDB% /OPT:REF /OPT:ICF)

# Unit tests of the parts that need no game (the Unreal profile's views, the core's Match=Camera choice):
# tools/test-gyro4all.sh builds and runs them. Never shipped.
add_executable(gyro4all_tests ../tests/profile_tests.cpp engines/unrealEngine/ue_profile.cpp core/settings.cpp core/log.cpp core/stick_fallback.cpp core/stick_response.cpp core/stick_camera.cpp)
target_include_directories(gyro4all_tests PRIVATE core engines/unrealEngine "${MINHOOK_DIR}/src/hde")
target_link_libraries(gyro4all_tests PRIVATE minhook)
target_compile_definitions(gyro4all_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                           "GYRO4ALL_VERSION=\"${PROJECT_VERSION}\"")
target_compile_options(gyro4all_tests PRIVATE /W4 /permissive- /utf-8)
