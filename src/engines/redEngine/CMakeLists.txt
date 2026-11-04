# REDengine module (CD Projekt RED): Cyberpunk 2077 (REDengine 4). Detection, camera identification and the on-foot
# first-person view written directly (body yaw or, seated, look yaw; look pitch); zoom FOV.
add_library(gyro_red OBJECT red_engine.cpp)
target_include_directories(gyro_red PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_red PRIVATE gyro_options gyro_core)
