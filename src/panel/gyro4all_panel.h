#pragma once
// The C interface between winmm.dll (Gyro4All's proxy, static CRT) and Gyro4All\gyro4all.dll (dynamic CRT): GyroLib's
// static ImGui settings panel drawn into the game's own DX11, DX10, DX9, Vulkan or OpenGL frames. winmm.dll loads gyro4all.dll
// only for those games (DX12 games use GyroLib's DX12 frontend inside gyrolib.dll), once the panel is enabled in
// gyrolib.ini. Only fixed-width types, COM pointers and Vulkan handles cross it; nothing allocated on one side is freed
// on the other.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G4A_ABI_VERSION 5u

struct gl_context;

enum { G4A_OK = 0, G4A_UNAVAILABLE = 1, G4A_FAILED = 2 };

// Same bits as GyroLib's GL_OVERLAY_CAPTURE_*.
enum { G4A_CAPTURE_MOUSE = 1u, G4A_CAPTURE_KEYBOARD = 2u, G4A_CAPTURE_GAMEPAD = 4u };

// The controller GyroLib selected, for the panel's gamepad navigation (bits of g4a_gamepad.buttons).
enum {
    G4A_PAD_SOUTH = 1u << 0, G4A_PAD_EAST = 1u << 1, G4A_PAD_WEST = 1u << 2, G4A_PAD_NORTH = 1u << 3,
    G4A_PAD_BACK = 1u << 4, G4A_PAD_START = 1u << 5, G4A_PAD_LEFT_STICK = 1u << 6, G4A_PAD_RIGHT_STICK = 1u << 7,
    G4A_PAD_LEFT_SHOULDER = 1u << 8, G4A_PAD_RIGHT_SHOULDER = 1u << 9, G4A_PAD_DPAD_UP = 1u << 10,
    G4A_PAD_DPAD_DOWN = 1u << 11, G4A_PAD_DPAD_LEFT = 1u << 12, G4A_PAD_DPAD_RIGHT = 1u << 13,
};
enum { G4A_AXIS_LEFTX, G4A_AXIS_LEFTY, G4A_AXIS_RIGHTX, G4A_AXIS_RIGHTY, G4A_AXIS_LEFT_TRIGGER, G4A_AXIS_RIGHT_TRIGGER,
       G4A_AXIS_COUNT };

typedef struct g4a_gamepad {
    uint32_t connected;
    uint32_t buttons;              // G4A_PAD_* bits
    int16_t axes[G4A_AXIS_COUNT];  // SDL ranges: sticks -32768..32767 (down = +Y), triggers 0..32767
} g4a_gamepad;

// One line of text for Gyro4All.log (winmm.dll prefixes it with "panel: "). Any thread.
typedef void (*g4a_log_fn)(const char* utf8_line);

#define G4A_COLOR_SPACE_UNKNOWN UINT32_MAX

typedef struct g4a_api {
    uint32_t size, abi_version;

    // Owner thread (the game thread, the one that calls gl_update). create: the ImGui context, its Win32 platform
    // backend on the game window and GyroLib's panel. frame: once per gyro frame, after gl_update; builds the panel's
    // ImGui frame while it is open (nothing at all while it is closed).
    int32_t (*create)(struct gl_context* ctx, void* hwnd, g4a_log_fn log);
    int32_t (*function_key)(uint32_t number, uint32_t pressed, uint32_t repeat);  // F1..F24 as 1..24
    int32_t (*frame)(const g4a_gamepad* pad);

    // The game window's thread, every message while the panel may be open. Returns the G4A_CAPTURE_* bits that apply
    // to this message: the caller keeps it from the game (and cleans up a consumed WM_INPUT).
    uint32_t (*window_message)(void* hwnd, uint32_t message, uint64_t wparam, int64_t lparam);

    // Any thread. capture: G4A_CAPTURE_* while the panel is open. ready: a renderer is initialized and the game
    // presented within the last 250 ms (GyroLib closes its DX12 panel without that, and so does this one).
    uint32_t (*capture)(void);
    uint32_t (*ready)(void);

    // The game's presenting threads, inside its own calls, before the original. DXGI (D3D10 or D3D11 swapchain):
    // color_space is what the game last passed to SetColorSpace1, or G4A_COLOR_SPACE_UNKNOWN.
    int32_t (*dxgi_present)(void* swapchain, uint32_t color_space);
    void (*dxgi_before_resize)(void* swapchain);
    // D3D9: IDirect3DDevice9* (Present, PresentEx or the implicit swapchain's Present) and around Reset/ResetEx.
    int32_t (*dx9_present)(void* device);
    void (*dx9_before_reset)(void* device);

    // Vulkan, inside the game's vkQueuePresentKHR (before the original) for the panel's swapchain: the panel is drawn
    // into the image being presented. On G4A_OK with signal_semaphore set, the present must wait on that semaphore
    // instead of the game's wait semaphores (the panel's submission waited on those). Inside vkDestroySwapchainKHR and
    // vkDestroyDevice (before the original): everything the panel made for them is destroyed.
    int32_t (*vk_present)(struct g4a_vk_present* present);
    void (*vk_swapchain_destroyed)(uint64_t swapchain);
    void (*vk_device_destroyed)(void* device);

    // OpenGL, inside the game's SwapBuffers/wglSwapBuffers (before the original), with the HDC being swapped: the panel
    // is drawn into that back buffer with the game's GL context current on this thread (nothing when none is).
    int32_t (*gl_present)(void* hdc);

    // Owner thread: the game presents to another window now (a launcher's splash came first): the panel's input and
    // its shortcut follow it.
    int32_t (*set_window)(void* hwnd);
} g4a_api;

// One present of the panel's Vulkan swapchain. Handles as Vulkan's (dispatchable ones as pointers, the others as
// uint64_t), and the game's vkGetInstanceProcAddr, from which gyro4all.dll loads every function it uses.
typedef struct g4a_vk_present {
    uint32_t size;
    uint32_t api_version;            // VkApplicationInfo::apiVersion the game created its instance with
    void* get_instance_proc_addr;    // PFN_vkGetInstanceProcAddr
    void* instance;                  // VkInstance
    void* physical_device;           // VkPhysicalDevice
    void* device;                    // VkDevice
    void* queue;                     // VkQueue the game presents on
    uint32_t queue_family;
    uint32_t image_index;
    uint64_t swapchain;              // VkSwapchainKHR
    uint32_t image_count;
    const uint64_t* images;          // VkImage[image_count], the swapchain's
    uint32_t format;                 // VkFormat of the images
    uint32_t width, height;          // their extent
    uint32_t wait_semaphore_count;
    const uint64_t* wait_semaphores; // VkSemaphore[]: the game's, from VkPresentInfoKHR
    uint64_t signal_semaphore;       // out: VkSemaphore the present must wait on instead (0 = unchanged)
    // ABI 5. A queue of the device's first graphics family the game got (null if none), for presents from a queue that
    // can't draw (DOOM 2016 on AMD presents from its compute queue). The game may use it on another thread meanwhile:
    // gyro4all.dll submits there only between lock_queue(queue, 1) and lock_queue(queue, 0), which winmm.dll also holds
    // around the game's own submissions and waits on that queue.
    void* draw_queue;                // VkQueue
    uint32_t draw_queue_family;
    uint32_t concurrent;             // the swapchain's images are VK_SHARING_MODE_CONCURRENT (no ownership transfers)
    void (*lock_queue)(void* queue, int32_t lock);
} g4a_vk_present;

// The DLL's only export.
typedef const g4a_api* (*g4a_get_api_fn)(uint32_t abi_version);
const g4a_api* g4a_get_api(uint32_t abi_version);

#ifdef __cplusplus
}
#endif
