#pragma once
// The settings panel in Vulkan games (panel_vk.cpp): import hooks on vulkan-1.dll that find the game's swapchain and
// hand its presents to gyro4all.dll.

namespace gyro4all::panel_vk {

// DllMain, with the panel's other hooks (panel_host::install).
void install();

}  // namespace gyro4all::panel_vk
