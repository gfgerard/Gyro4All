# PlatinumGames module: NieR:Automata. The gyro turns the orbit camera's angles in a hook on the game's camera update.
add_library(gyro_platinum OBJECT platinum_engine.cpp)
target_include_directories(gyro_platinum PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_platinum PRIVATE gyro_options gyro_core)
