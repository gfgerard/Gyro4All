# 4A Engine module (4A Games): Metro 2033 Redux, Metro: Last Light Redux. The view through the player's pitch/yaw floats.
add_library(gyro_foura OBJECT foura_engine.cpp)
target_include_directories(gyro_foura PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_foura PRIVATE gyro_options gyro_core)
