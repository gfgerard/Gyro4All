# Frostbite (EA) module: runtime TypeInfo reader (layouts as in Frosty Tool Suite). Dead Space 2023: detection, frame, type dump, camera model discovery (deadspace_camera.cpp); no camera turned yet.
add_library(gyro_frostbite OBJECT fb_types.cpp fb_engine.cpp fb_xinput.cpp deadspace_camera.cpp bf4_camera.cpp mea_camera.cpp)
target_include_directories(gyro_frostbite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gyro_frostbite PRIVATE gyro_options gyro_core)
