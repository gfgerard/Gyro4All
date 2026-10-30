# Northlight module (Remedy): Control (the gameplay camera's look direction, control_camera.cpp), Alan Wake 2 (its camera
# state's, aw2_camera.cpp; both through look_queue.cpp), CONTROL Resonant (detection only; the gyro as right stick).
add_library(gyro_northlight OBJECT northlight_engine.cpp control_camera.cpp aw2_camera.cpp look_queue.cpp)
target_include_directories(gyro_northlight PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_northlight PRIVATE gyro_options gyro_core)
