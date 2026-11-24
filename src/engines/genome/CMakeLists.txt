# Genome engine module (Piranha Bytes): ELEX. Detection, the camera admin and the player camera's angles.
add_library(gyro_genome OBJECT genome_engine.cpp)
target_include_directories(gyro_genome PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_genome PRIVATE gyro_options gyro_core)
