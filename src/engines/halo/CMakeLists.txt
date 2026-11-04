# Halo module: Halo: The Master Chief Collection. Every game through its player control; the gyro as right stick when not found.
add_library(gyro_halo OBJECT halo_engine.cpp)
target_include_directories(gyro_halo PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_halo PRIVATE gyro_options gyro_core)
