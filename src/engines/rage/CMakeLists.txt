# RAGE module (Rockstar): Red Dead Redemption 2. No camera hook yet: the gyro as right stick (XInput).
add_library(gyro_rage OBJECT rage_engine.cpp)
target_include_directories(gyro_rage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_rage PRIVATE gyro_options gyro_core)
