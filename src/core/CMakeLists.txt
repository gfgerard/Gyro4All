# Engine-neutral core. It must not include anything from engines/ (no include path to it here).
add_library(gyro_core OBJECT dllmain.cpp proxy.cpp host.cpp engine.cpp settings.cpp hooks.cpp overlay.cpp perf.cpp log.cpp
            notify.cpp deps.cpp panel_host.cpp panel_vk.cpp panel_gl.cpp xinput.cpp xinput_wmi.cpp nativepad.cpp dinputpad.cpp stick_fallback.cpp stick_response.cpp stick_camera.cpp sensor_pacer.cpp longpress.cpp steammouse.cpp mem.cpp
            writewatch.cpp)
target_include_directories(gyro_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# gyro4all.dll's C interface (src/panel/gyro4all_panel.h): the core loads that DLL, it never links it.
target_include_directories(gyro_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../panel")
# Vulkan's headers (third_party/Vulkan-Headers-<version>, see MODLOG): the panel's Vulkan hooks (panel_vk.cpp).
target_include_directories(gyro_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/Vulkan-Headers-1.4.309/include")
target_link_libraries(gyro_core PRIVATE gyro_options)
