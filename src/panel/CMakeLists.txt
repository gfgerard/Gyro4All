# gyro4all.dll: GyroLib's static ImGui panel drawn into DX11, DX10, DX9, Vulkan and OpenGL games (see gyro4all_panel.h). GyroLib ships
# that panel (and its ImGui) built against the DLL C++ runtime (/MD), which winmm.dll (/MT) can't link: hence this DLL,
# loaded by winmm.dll only when a game needs it. ImGui's platform and renderer backends come from the exact ImGui tag
# GyroLib bundles (third_party/imgui-<version>, see MODLOG); its headers are GyroLib's.
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/imgui-1.92.9b")
add_library(gyro4all_panel SHARED panel_main.cpp panel_dx11.cpp panel_dx10.cpp panel_dx9.cpp panel_vk.cpp panel_gl.cpp gyro4all.def
            "${IMGUI_DIR}/backends/imgui_impl_win32.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
            "${IMGUI_DIR}/backends/imgui_impl_dx10.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx9.cpp"
            "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp" "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
set_target_properties(gyro4all_panel PROPERTIES OUTPUT_NAME gyro4all MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
# Backends include "imgui.h": GyroLib's copy (the one its library was built from), found through GyroLib::gyrolib_imgui.
target_include_directories(gyro4all_panel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${IMGUI_DIR}/backends")
# Vulkan's headers (third_party/Vulkan-Headers-<version>) for ImGui's Vulkan backend, whose functions come from the game's
# own vkGetInstanceProcAddr (IMGUI_IMPL_VULKAN_NO_PROTOTYPES: no import of vulkan-1.dll).
target_include_directories(gyro4all_panel PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/Vulkan-Headers-1.4.309/include")
# The game's XInput is Gyro4All's; the panel's gamepad is GyroLib's selected controller, passed by winmm.dll.
target_compile_definitions(gyro4all_panel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                           IMGUI_IMPL_WIN32_DISABLE_GAMEPAD IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_compile_options(gyro4all_panel PRIVATE /W3 /permissive- /utf-8 /Zi)
target_link_libraries(gyro4all_panel PRIVATE GyroLib::gyrolib_panel d3dcompiler dxguid delayimp)
# gyrolib.dll is already loaded by winmm.dll (from the Gyro4All folder) whenever this DLL is. The shader compiler
# (System32, used by the DX10/DX11 backends and the HDR composition) loads only when one of those renderers starts.
target_link_options(gyro4all_panel PRIVATE /DEBUG /PDBALTPATH:%_PDB% /OPT:REF /OPT:ICF /DELAYLOAD:d3dcompiler_47.dll)
