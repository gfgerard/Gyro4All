# Source engine module (Valve; Respawn's branch): Titanfall 2. The view through the engine client's GetViewAngles/SetViewAngles.
add_library(gyro_source OBJECT source_engine.cpp)
target_include_directories(gyro_source PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_source PRIVATE gyro_options gyro_core)
