# Asura module (Rebellion): Zombie Army 4: Dead War. The view through the local player's look direction.
add_library(gyro_asura OBJECT asura_engine.cpp)
target_include_directories(gyro_asura PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_asura PRIVATE gyro_options gyro_core)
