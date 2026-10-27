# Unreal Engine 3 module: Mass Effect Legendary Edition first. Reflection found at run time; the view through Controller.Rotation.
add_library(gyro_unreal3 OBJECT ue3_engine.cpp ue3_xinput.cpp)
target_include_directories(gyro_unreal3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_unreal3 PRIVATE gyro_options gyro_core)
