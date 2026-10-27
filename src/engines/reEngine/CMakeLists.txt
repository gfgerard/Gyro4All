# RE Engine (Capcom) module: runtime TDB reader, via.Application frame hook, camera models per game family.
add_library(gyro_re OBJECT re_tdb.cpp re_camera.cpp re7_camera.cpp re2_camera.cpp re4_camera.cpp requiem_camera.cpp pragmata_camera.cpp
            mhwilds_camera.cpp re_engine.cpp)
target_include_directories(gyro_re PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_re PRIVATE gyro_options gyro_core)
