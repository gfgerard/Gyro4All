# Foundation engine module (Crystal Dynamics / Eidos Montreal): Shadow of the Tomb Raider. Detection and the gyro frame
# and the third-person camera (ExplorationCameraController yaw and pitch).
add_library(gyro_foundation OBJECT foundation_engine.cpp)
target_include_directories(gyro_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_foundation PRIVATE gyro_options gyro_core)
