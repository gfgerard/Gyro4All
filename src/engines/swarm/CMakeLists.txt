# Swarm Engine module (Saber Interactive): Warhammer 40,000: Space Marine 2. The view through the camera controller's angles.
add_library(gyro_swarm OBJECT swarm_engine.cpp)
target_include_directories(gyro_swarm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_swarm PRIVATE gyro_options gyro_core)
