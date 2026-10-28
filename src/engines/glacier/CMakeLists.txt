# Glacier 2 module (IO Interactive): HITMAN 3 (World of Assassination). The local player and the game clock from the
# engine's globals; the view through the third-person camera controller's angles.
add_library(gyro_glacier OBJECT glacier_engine.cpp)
target_include_directories(gyro_glacier PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_glacier PRIVATE gyro_options gyro_core)
