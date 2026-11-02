#pragma once
// The C interface between winmm.dll (Gyro4All's proxy, static CRT) and Gyro4All\gyro4all.dll (dynamic CRT): GyroLib's
// static ImGui settings panel plus Gyro4All's own settings window, drawn into the game's own DX11, DX10, DX9, Vulkan,
// OpenGL or (ABI 6) DX12 frames. winmm.dll loads gyro4all.dll once the panel is enabled in gyrolib.ini; in DX12 games
// GyroLib draws its own panel (its DX12 frontend inside gyrolib.dll) and this DLL draws only the Gyro4All window, after
// GyroLib's, in the game's Present. Only fixed-width types, COM pointers and Vulkan handles cross it; nothing allocated
// on one side is freed on the other. The two DLLs ship in the same release, so both sides of this interface are always
// the same build: new fields are appended and G4A_ABI_VERSION is bumped; no cross-version support.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G4A_ABI_VERSION 7u

struct gl_context;

enum { G4A_OK = 0, G4A_UNAVAILABLE = 1, G4A_FAILED = 2 };

// Same bits as GyroLib's GL_OVERLAY_CAPTURE_*.
enum { G4A_CAPTURE_MOUSE = 1u, G4A_CAPTURE_KEYBOARD = 2u, G4A_CAPTURE_GAMEPAD = 4u };

// window_message may also set these: while one holds, winmm.dll keeps the matching messages from reaching ANOTHER
// frontend on the same window (GyroLib's own panel, drawn underneath this DLL's window) — the mouse over one of this
// DLL's windows (or one of its items held), the keyboard while one of its windows is focused. Unlike G4A_CAPTURE_*,
// which decides what the game does not get, these decide which panel sees the message.
enum { G4A_CLAIM_MOUSE = 0x100u, G4A_CLAIM_KEYBOARD = 0x200u };

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

// The Gyro4All window's settings access (ABI 6): winmm.dll owns Gyro4All.ini. Keys and values are utf-8; nothing
// allocated on one side is freed on the other. get: the effective value (the game's profile wins over Gyro4All.ini,
// as at run time), copied into out (NUL-terminated, truncated to out_size); 1 when set, 0 when the key has no value.
// set: writes key=value into Gyro4All.ini (never the profile) so it persists; winmm applies what it can live.
typedef struct g4a_settings {
    uint32_t size, abi_version;
    uint32_t (*get)(const char* key, char* out, uint32_t out_size);
    int32_t (*set)(const char* key, const char* value);
} g4a_settings;

#define G4A_COLOR_SPACE_UNKNOWN UINT32_MAX

// create: flags bit 0 (G4A_CREATE_DX12): a DX12 game. GyroLib's panel is its DX12 frontend in gyrolib.dll; this DLL
// draws no gl_panel (gl_panel_draw is never called), only the Gyro4All window, and renders it through dx12_present.
enum { G4A_CREATE_DX12 = 1u };

typedef struct g4a_api {
    uint32_t size, abi_version;

    // Owner thread (the game thread, the one that calls gl_update). create: the ImGui context, its Win32 platform
    // backend on the game window, the settings access and GyroLib's panel (not in a DX12 game, G4A_CREATE_DX12).
    // frame: once per gyro frame, after gl_update; builds the panel's ImGui frame while it is open (nothing at all
    // while it is closed).
    int32_t (*create)(struct gl_context* ctx, void* hwnd, g4a_log_fn log, const g4a_settings* settings,
                      uint32_t flags);
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

    // ABI 6, DX12 games (G4A_CREATE_DX12). dx12_present: the game's Present, inside the call on the render worker,
    // after GyroLib's DX12 overlay rendered on the same queue: draw the Gyro4All window into the current backbuffer.
    // swapchain: IDXGISwapChain3*, command_queue: ID3D12CommandQueue* (DIRECT, same device), color_space: the last
    // DXGI_COLOR_SPACE_TYPE the game passed to SetColorSpace1, or G4A_COLOR_SPACE_UNKNOWN. No work while the window
    // has no frame. dx12_before_resize: before the game's ResizeBuffers: release every reference to the swapchain's
    // buffers. dx12_lost: the captured DX12 swapchain is released or replaced: the renderer shuts down (a later
    // dx12_present re-initializes it).
    int32_t (*dx12_present)(void* swapchain, void* command_queue, uint32_t color_space);
    void (*dx12_before_resize)(void* swapchain);
    void (*dx12_lost)(void);

    // ABI 7, any game. toggle_expanded: fold/unfold the Gyro4All settings window (winmm.dll calls it on the L3+R3
    // gamepad chord while the panel is open; ImGui's gamepad nav cannot cross windows, so the chord drives it).
    // The panel moves ImGui's nav focus to the window (or back to GyroLib's panel window when folding, in the
    // games where that window is in this DLL's ImGui context).
    int32_t (*toggle_expanded)();
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
