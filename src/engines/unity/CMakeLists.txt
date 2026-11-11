# Unity module (Mono scripting backend): MOUSE: P.I. For Hire. The camera from the game's profile ([Unity] member paths),
# read and turned through the embedded Mono runtime's API.
add_library(gyro_unity OBJECT unity_engine.cpp unity_mono.cpp)
target_include_directories(gyro_unity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_unity PRIVATE gyro_options gyro_core)
