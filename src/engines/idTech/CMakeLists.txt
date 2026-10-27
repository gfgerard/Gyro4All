# id Tech module (id Software): DOOM (2016) first. Type info and RTTI read at runtime (idtech_types.cpp), gameLocal and the
# local player found in the heap; the gyro is added to the player's deltaViewAngles.
add_library(gyro_idtech OBJECT idtech_types.cpp idtech_engine.cpp)
target_include_directories(gyro_idtech PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_idtech PRIVATE gyro_options gyro_core)
