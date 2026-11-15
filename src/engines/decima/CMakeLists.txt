# Decima engine module (Guerrilla Games; Kojima Productions): Death Stranding (Director's Cut). The gyro turns the
# active camera mode's pitch and heading.
add_library(gyro_decima OBJECT decima_engine.cpp)
target_include_directories(gyro_decima PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_decima PRIVATE gyro_options gyro_core)
