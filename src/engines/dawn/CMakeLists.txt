# Dawn engine module (Eidos Montréal, a Glacier 2 fork): Deus Ex: Mankind Divided. The gyro turns the player character's
# own yaw and pitch.
add_library(gyro_dawn OBJECT dawn_engine.cpp)
target_include_directories(gyro_dawn PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_dawn PRIVATE gyro_options gyro_core)
