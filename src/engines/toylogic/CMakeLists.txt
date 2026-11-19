# Toylogic engine module: NieR Replicant ver.1.22474487139. The camera's yaw and pitch, static floats found by a code signature.
add_library(gyro_toylogic OBJECT toylogic_engine.cpp)
target_include_directories(gyro_toylogic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_toylogic PRIVATE gyro_options gyro_core)
