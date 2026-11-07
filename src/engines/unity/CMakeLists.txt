add_library(gyro_unity OBJECT unity_runtime.cpp unity_path.cpp unity_engine.cpp unity_profile.cpp unity_probe.cpp)
target_include_directories(gyro_unity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_unity PRIVATE gyro_options gyro_core)
