# CryEngine module (Crytek): Crysis (2007), Crysis Remastered and Kingdom Come: Deliverance. The gyro joins the local player's movement request.
add_library(gyro_cryengine OBJECT cryengine_engine.cpp)
target_include_directories(gyro_cryengine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_cryengine PRIVATE gyro_options gyro_core)
