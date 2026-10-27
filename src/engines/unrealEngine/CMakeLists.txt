# Unreal Engine (UE4.25+ / UE5) module: reflection reader, scene, profile views, camera probe.
add_library(gyro_unreal OBJECT ue.cpp ue_engine.cpp ue_probe.cpp ue_profile.cpp ue_update_rotation.cpp)
target_include_directories(gyro_unreal PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_unreal PRIVATE gyro_options gyro_core)
# MinHook's instruction length decoder (hde64), for finding UpdateRotation by its shape.
target_include_directories(gyro_unreal PRIVATE "${MINHOOK_DIR}/src/hde")
