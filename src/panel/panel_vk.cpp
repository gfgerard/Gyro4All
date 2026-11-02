// The panel's Vulkan renderer: ImGui's Vulkan backend drawing into the game's swapchain image inside its
// vkQueuePresentKHR (winmm.dll's panel_vk.cpp hands each present over). Every Vulkan function comes from the game's
// own vkGetInstanceProcAddr (no Vulkan import of our own).
//
// Per device: a render pass that keeps the game's image (load, store; PRESENT_SRC_KHR before and after) in the
// swapchain's format, and ImGui's backend on it. Per swapchain image: an image view, a framebuffer, a command pool and
// buffer, a fence and a semaphore. A present while the panel shows: wait for that image's previous panel submission,
// record the panel's draw, submit it on the game's present queue waiting on the game's semaphores and signalling ours;
// the present then waits on ours. While the panel is closed nothing is recorded or submitted.
//
// HDR swapchains (scRGB, HDR10 color spaces): the render pass gets a second subpass that composes the panel, drawn in SDR
// into an attachment of its own, over the game's image in linear light (Mode, make_render_pass, make_composite). Its
// shaders are SPIR-V built from shaders/panel_vk_hdr.hlsl (tools/compile-vk-shaders.sh).
//
// A present from a queue that can't draw (DOOM 2016 on AMD presents from its compute queue in game) is drawn on the
// game's graphics queue that winmm.dll hands over (g4a_vk_present::draw_queue, submitted to under its lock_queue);
// with exclusive swapchain images, the present's queue releases the image to that family and acquires it back after
// the draw, in submissions of their own around the draw's (per image: a second command pool, two more semaphores).
//
// Nothing here waits on the game's queue: ImGui's backend uploads its textures (the font atlas) with a vkQueueWaitIdle
// on the queue it was given, which froze DOOM 2016 inside its own vkQueuePresentKHR (the queue held the game's work
// that only completes after the present). So the panel handles ImGui's texture requests itself (ImDrawData::Textures
// = nullptr for the backend): uploads are recorded into the panel's own command buffer, before its render pass, and
// their staging buffers freed once every panel submission's fence signalled.
#include "panel.hpp"
#include "panel_vk_shaders.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace g4a::vk {
namespace {

struct Fns {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_family_properties = nullptr;
    PFN_vkCreateRenderPass create_render_pass = nullptr;
    PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateFramebuffer create_framebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
    PFN_vkCreateCommandPool create_command_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
    PFN_vkResetCommandPool reset_command_pool = nullptr;
    PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
    PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
    PFN_vkEndCommandBuffer end_command_buffer = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_render_pass = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkWaitForFences wait_for_fences = nullptr;
    PFN_vkResetFences reset_fences = nullptr;
    PFN_vkCreateSemaphore create_semaphore = nullptr;
    PFN_vkDestroySemaphore destroy_semaphore = nullptr;
    PFN_vkQueueSubmit queue_submit = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements image_requirements = nullptr;
    PFN_vkBindImageMemory bind_image_memory = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements buffer_requirements = nullptr;
    PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
    PFN_vkAllocateMemory allocate_memory = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkMapMemory map_memory = nullptr;
    PFN_vkUnmapMemory unmap_memory = nullptr;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buffer_to_image = nullptr;
    // HDR composition.
    PFN_vkCreateShaderModule create_shader_module = nullptr;
    PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
    PFN_vkCreateDescriptorSetLayout create_set_layout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_set_layout = nullptr;
    PFN_vkCreateDescriptorPool create_descriptor_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_descriptor_sets = nullptr;
    PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
    PFN_vkCreateGraphicsPipelines create_graphics_pipelines = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCmdNextSubpass cmd_next_subpass = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = nullptr;
    PFN_vkCmdPushConstants cmd_push_constants = nullptr;
    PFN_vkCmdSetViewport cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor cmd_set_scissor = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;
};

// A texture ImGui asked for (ImTextureData::BackendUserData), made and uploaded by the panel itself.
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // ImGui's backend's descriptor for it (the ImTextureID)
    bool uploaded = false;                 // in SHADER_READ_ONLY layout since its first upload
};
struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Frame {
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;   // signalled when this image's last panel submission finished
    VkSemaphore done = VK_NULL_HANDLE;
    // Only when the present's queue can't draw and the images are exclusive: the ownership transfers to the draw queue
    // and back, recorded on the present's family.
    VkCommandPool present_pool = VK_NULL_HANDLE;
    VkCommandBuffer release = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
    VkSemaphore released = VK_NULL_HANDLE, drawn = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // HDR: the composition's input attachments (the panel; HDR10: this image)
};

// How the panel reaches the swapchain's image. Sdr: ImGui draws on it directly. ScRgb / Hdr10: ImGui draws into the
// panel's own SDR attachment (subpass 0), which a second subpass composes over the image in linear light with the UI
// white at 203 nits, as panel_dx11.cpp does. Hdr10 reads the game's pixel as an input attachment (winmm.dll adds that
// usage); Hdr10Pq, when the driver refused it, blends in PQ instead.
enum class Mode { Sdr, ScRgb, Hdr10, Hdr10Pq };
const char* name(Mode m) {
    return m == Mode::Sdr     ? "SDR"
           : m == Mode::ScRgb ? "scRGB"
           : m == Mode::Hdr10 ? "HDR10"
                              : "HDR10, blended in PQ (no input-attachment usage)";
}
constexpr float kWhiteNits = 203.0f;  // GyroLib's default UI white in HDR

Mode pick_mode(uint32_t color_space, bool input_attachment) {
    if (color_space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) return Mode::ScRgb;
    if (color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT) return input_attachment ? Mode::Hdr10 : Mode::Hdr10Pq;
    return Mode::Sdr;
}

// Under g4a::g_lock.
Fns f;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;  // the queue the panel draws on: the present's, or the game's graphics queue
uint32_t g_family = 0;             // its family: the frames' draw command pools are made for it
uint32_t g_present_family = 0;     // the present's family (ownership transfers when not g_family)
uint32_t g_frames_present_family = UINT32_MAX;  // the one g_frames were made for
std::vector<VkQueueFlags> g_family_flags;
VkFormat g_format = VK_FORMAT_UNDEFINED;
VkRenderPass g_render_pass = VK_NULL_HANDLE;
bool g_backend = false;
bool g_failed = false;  // this device can't show the panel (logged once)
uint64_t g_swapchain = 0;
std::vector<uint64_t> g_images;  // the swapchain images g_frames were made for
uint32_t g_width = 0, g_height = 0;
std::vector<Frame> g_frames;
std::vector<Staging> g_staging;  // uploads of the last submission, freed once every panel fence signalled
VkPhysicalDeviceMemoryProperties g_memory{};
// HDR composition (Mode other than Sdr). The panel attachment is one image for every frame: present() waits for all
// earlier panel submissions before recording.
Mode g_mode = Mode::Sdr;
uint32_t g_color_space = 0;
bool g_input_attachment = false;
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkPipeline g_composite = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;  // the frames' sets, made again with them
VkImage g_panel_image = VK_NULL_HANDLE;
VkDeviceMemory g_panel_memory = VK_NULL_HANDLE;
VkImageView g_panel_view = VK_NULL_HANDLE;

template <class T>
bool load(T& out, const char* name, bool device) {
    out = reinterpret_cast<T>(device ? f.get_device_proc(g_device, name) : f.get_instance_proc(g_instance, name));
    if (!out) log("Vulkan: %s not available: panel unavailable", name);
    return out != nullptr;
}

bool load_functions() {
    if (!load(f.get_device_proc, "vkGetDeviceProcAddr", false)) return false;
    return load(f.queue_family_properties, "vkGetPhysicalDeviceQueueFamilyProperties", false) &&
           load(f.create_render_pass, "vkCreateRenderPass", true) &&
           load(f.destroy_render_pass, "vkDestroyRenderPass", true) &&
           load(f.create_image_view, "vkCreateImageView", true) &&
           load(f.destroy_image_view, "vkDestroyImageView", true) &&
           load(f.create_framebuffer, "vkCreateFramebuffer", true) &&
           load(f.destroy_framebuffer, "vkDestroyFramebuffer", true) &&
           load(f.create_command_pool, "vkCreateCommandPool", true) &&
           load(f.destroy_command_pool, "vkDestroyCommandPool", true) &&
           load(f.reset_command_pool, "vkResetCommandPool", true) &&
           load(f.allocate_command_buffers, "vkAllocateCommandBuffers", true) &&
           load(f.begin_command_buffer, "vkBeginCommandBuffer", true) &&
           load(f.end_command_buffer, "vkEndCommandBuffer", true) &&
           load(f.cmd_begin_render_pass, "vkCmdBeginRenderPass", true) &&
           load(f.cmd_end_render_pass, "vkCmdEndRenderPass", true) &&
           load(f.create_fence, "vkCreateFence", true) && load(f.destroy_fence, "vkDestroyFence", true) &&
           load(f.wait_for_fences, "vkWaitForFences", true) && load(f.reset_fences, "vkResetFences", true) &&
           load(f.create_semaphore, "vkCreateSemaphore", true) &&
           load(f.destroy_semaphore, "vkDestroySemaphore", true) && load(f.queue_submit, "vkQueueSubmit", true) &&
           load(f.device_wait_idle, "vkDeviceWaitIdle", true) &&
           load(f.memory_properties, "vkGetPhysicalDeviceMemoryProperties", false) &&
           load(f.create_image, "vkCreateImage", true) && load(f.destroy_image, "vkDestroyImage", true) &&
           load(f.image_requirements, "vkGetImageMemoryRequirements", true) &&
           load(f.bind_image_memory, "vkBindImageMemory", true) && load(f.create_buffer, "vkCreateBuffer", true) &&
           load(f.destroy_buffer, "vkDestroyBuffer", true) &&
           load(f.buffer_requirements, "vkGetBufferMemoryRequirements", true) &&
           load(f.bind_buffer_memory, "vkBindBufferMemory", true) &&
           load(f.allocate_memory, "vkAllocateMemory", true) && load(f.free_memory, "vkFreeMemory", true) &&
           load(f.map_memory, "vkMapMemory", true) && load(f.unmap_memory, "vkUnmapMemory", true) &&
           load(f.cmd_pipeline_barrier, "vkCmdPipelineBarrier", true) &&
           load(f.cmd_copy_buffer_to_image, "vkCmdCopyBufferToImage", true) &&
           load(f.create_shader_module, "vkCreateShaderModule", true) &&
           load(f.destroy_shader_module, "vkDestroyShaderModule", true) &&
           load(f.create_set_layout, "vkCreateDescriptorSetLayout", true) &&
           load(f.destroy_set_layout, "vkDestroyDescriptorSetLayout", true) &&
           load(f.create_descriptor_pool, "vkCreateDescriptorPool", true) &&
           load(f.destroy_descriptor_pool, "vkDestroyDescriptorPool", true) &&
           load(f.allocate_descriptor_sets, "vkAllocateDescriptorSets", true) &&
           load(f.update_descriptor_sets, "vkUpdateDescriptorSets", true) &&
           load(f.create_pipeline_layout, "vkCreatePipelineLayout", true) &&
           load(f.destroy_pipeline_layout, "vkDestroyPipelineLayout", true) &&
           load(f.create_graphics_pipelines, "vkCreateGraphicsPipelines", true) &&
           load(f.destroy_pipeline, "vkDestroyPipeline", true) && load(f.cmd_next_subpass, "vkCmdNextSubpass", true) &&
           load(f.cmd_bind_pipeline, "vkCmdBindPipeline", true) &&
           load(f.cmd_bind_descriptor_sets, "vkCmdBindDescriptorSets", true) &&
           load(f.cmd_push_constants, "vkCmdPushConstants", true) &&
           load(f.cmd_set_viewport, "vkCmdSetViewport", true) && load(f.cmd_set_scissor, "vkCmdSetScissor", true) &&
           load(f.cmd_draw, "vkCmdDraw", true);
}

int memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < g_memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_memory.memoryTypes[i].propertyFlags & flags) == flags) return int(i);
    return -1;
}

// What ImGui's backend will do with its vertex buffers (the first HOST_VISIBLE memory type the buffer allows, then
// vkMapMemory), tried once at start: if it fails, ImGui would write through a null mapping inside the game's present.
bool vertex_memory_maps() {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 4096;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (f.create_buffer(g_device, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    f.buffer_requirements(g_device, buffer, &req);
    const int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkResult r = type < 0 ? VK_ERROR_FEATURE_NOT_PRESENT : f.allocate_memory(g_device, &alloc, nullptr, &memory);
    if (r == VK_SUCCESS) r = f.bind_buffer_memory(g_device, buffer, memory, 0);
    if (r == VK_SUCCESS) r = f.map_memory(g_device, memory, 0, info.size, 0, &mapped);
    if (r == VK_SUCCESS && mapped) f.unmap_memory(g_device, memory);
    if (memory) f.free_memory(g_device, memory, nullptr);
    f.destroy_buffer(g_device, buffer, nullptr);
    std::string types;
    for (uint32_t i = 0; i < g_memory.memoryTypeCount; ++i)
        types += " " + std::to_string(i) + ":" + std::to_string(g_memory.memoryTypes[i].propertyFlags);
    log("Vulkan: memory types (index:flags)%s; vertex buffers use type %d: %s (%d)", types.c_str(), type,
        r == VK_SUCCESS && mapped ? "maps" : "does NOT map", int(r));
    return r == VK_SUCCESS && mapped;
}

void free_staging() {
    for (const Staging& st : g_staging) {
        if (st.buffer) f.destroy_buffer(g_device, st.buffer, nullptr);
        if (st.memory) f.free_memory(g_device, st.memory, nullptr);
    }
    g_staging.clear();
}

void destroy_texture(ImTextureData* tex) {
    if (auto* t = static_cast<Texture*>(tex->BackendUserData)) {
        if (t->set) ImGui_ImplVulkan_RemoveTexture(t->set);
        if (t->view) f.destroy_image_view(g_device, t->view, nullptr);
        if (t->image) f.destroy_image(g_device, t->image, nullptr);
        if (t->memory) f.free_memory(g_device, t->memory, nullptr);
        delete t;
    }
    tex->BackendUserData = nullptr;
    tex->SetTexID(ImTextureID_Invalid);
    tex->SetStatus(ImTextureStatus_Destroyed);
}

// Every texture this renderer made; before ImGui's backend shuts down, which would take them for its own.
void destroy_textures() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->BackendUserData) destroy_texture(tex);
}

// Creates or (wholly) updates one texture ImGui asked for, recording its upload into cmd. Every earlier panel
// submission is done (present() waited for all fences), so a texture can be destroyed or overwritten right away.
bool upload_texture(ImTextureData* tex, VkCommandBuffer cmd) {
    if (tex->Status == ImTextureStatus_WantDestroy) {
        destroy_texture(tex);
        return true;
    }
    if (tex->Format != ImTextureFormat_RGBA32) {
        log("Vulkan: ImGui texture format %d not supported", int(tex->Format));
        return false;
    }
    auto* t = static_cast<Texture*>(tex->BackendUserData);
    if (tex->Status == ImTextureStatus_WantCreate || !t) {
        if (t) destroy_texture(tex);
        t = new Texture;
        tex->BackendUserData = t;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {uint32_t(tex->Width), uint32_t(tex->Height), 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (f.create_image(g_device, &image, nullptr, &t->image) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        f.image_requirements(g_device, t->image, &req);
        const int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = uint32_t(type);
        if (type < 0 || f.allocate_memory(g_device, &alloc, nullptr, &t->memory) != VK_SUCCESS ||
            f.bind_image_memory(g_device, t->image, t->memory, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = t->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (f.create_image_view(g_device, &view, nullptr, &t->view) != VK_SUCCESS) return false;
        t->set = ImGui_ImplVulkan_AddTexture(t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!t->set) return false;
    }
    // A staging buffer with the whole texture.
    const VkDeviceSize size = VkDeviceSize(tex->Width) * VkDeviceSize(tex->Height) * 4;
    Staging st;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (f.create_buffer(g_device, &buffer, nullptr, &st.buffer) != VK_SUCCESS) return false;
    g_staging.push_back(st);
    Staging& staged = g_staging.back();
    VkMemoryRequirements req{};
    f.buffer_requirements(g_device, staged.buffer, &req);
    const int type =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    void* mapped = nullptr;
    if (type < 0 || f.allocate_memory(g_device, &alloc, nullptr, &staged.memory) != VK_SUCCESS ||
        f.bind_buffer_memory(g_device, staged.buffer, staged.memory, 0) != VK_SUCCESS ||
        f.map_memory(g_device, staged.memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return false;
    std::memcpy(mapped, tex->GetPixels(), size_t(size));
    f.unmap_memory(g_device, staged.memory);

    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.srcAccessMask = t->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_copy.oldLayout = t->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = t->image;
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(tex->Width), uint32_t(tex->Height), 1};
    f.cmd_copy_buffer_to_image(cmd, staged.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_read = to_copy;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    f.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_read);
    t->uploaded = true;
    tex->SetTexID(ImTextureID(t->set));
    tex->SetStatus(ImTextureStatus_OK);
    return true;
}

// ImGui's backend loads everything through the instance, as its default loader does: vkGetDeviceProcAddr may hand out
// a device-dispatch entry for an instance-level name (vkGetPhysicalDeviceMemoryProperties), which then read garbage
// memory properties, picked a memory type that can't be mapped, and ImGui's vertex upload wrote through a null
// mapping inside DOOM 2016's present (VK_ERROR_MEMORY_MAP_FAILED, then an access violation).
PFN_vkVoidFunction backend_loader(const char* name, void*) { return f.get_instance_proc(g_instance, name); }

void check(VkResult r) {
    if (r < 0) log("Vulkan: ImGui's backend got error %d", int(r));
}

void destroy_frames() {
    if (!g_device) return;
    // The panel's submissions on these images must be done. Only its own fences: this may run inside the game's
    // present (a new swapchain), where waiting for the whole device could wait on the game's queue.
    std::vector<VkFence> fences;
    for (const Frame& fr : g_frames)
        if (fr.fence) fences.push_back(fr.fence);
    if (!fences.empty() &&
        f.wait_for_fences(g_device, uint32_t(fences.size()), fences.data(), VK_TRUE, 1'000'000'000ull) != VK_SUCCESS)
        log("Vulkan: the panel's last submissions didn't finish in 1 s; its frames are destroyed anyway");
    for (Frame& fr : g_frames) {
        if (fr.framebuffer) f.destroy_framebuffer(g_device, fr.framebuffer, nullptr);
        if (fr.view) f.destroy_image_view(g_device, fr.view, nullptr);
        if (fr.pool) f.destroy_command_pool(g_device, fr.pool, nullptr);
        if (fr.fence) f.destroy_fence(g_device, fr.fence, nullptr);
        if (fr.done) f.destroy_semaphore(g_device, fr.done, nullptr);
        if (fr.present_pool) f.destroy_command_pool(g_device, fr.present_pool, nullptr);
        if (fr.released) f.destroy_semaphore(g_device, fr.released, nullptr);
        if (fr.drawn) f.destroy_semaphore(g_device, fr.drawn, nullptr);
    }
    g_frames.clear();
    g_images.clear();
    g_swapchain = 0;
    g_frames_present_family = UINT32_MAX;
    if (g_descriptor_pool) f.destroy_descriptor_pool(g_device, g_descriptor_pool, nullptr);  // frees the frames' sets
    if (g_panel_view) f.destroy_image_view(g_device, g_panel_view, nullptr);
    if (g_panel_image) f.destroy_image(g_device, g_panel_image, nullptr);
    if (g_panel_memory) f.free_memory(g_device, g_panel_memory, nullptr);
    g_descriptor_pool = VK_NULL_HANDLE;
    g_panel_view = VK_NULL_HANDLE;
    g_panel_image = VK_NULL_HANDLE;
    g_panel_memory = VK_NULL_HANDLE;
}

void destroy_composite() {
    if (g_composite) f.destroy_pipeline(g_device, g_composite, nullptr);
    if (g_pipeline_layout) f.destroy_pipeline_layout(g_device, g_pipeline_layout, nullptr);
    if (g_set_layout) f.destroy_set_layout(g_device, g_set_layout, nullptr);
    g_composite = VK_NULL_HANDLE;
    g_pipeline_layout = VK_NULL_HANDLE;
    g_set_layout = VK_NULL_HANDLE;
}

// in_present: inside the game's present, where only the panel's own submissions are waited for (destroy_frames); they
// are the only GPU work that uses the panel's objects.
void shutdown(bool in_present = false) {
    destroy_frames();
    if (g_device && f.device_wait_idle) {
        if (!in_present) f.device_wait_idle(g_device);
        free_staging();
        destroy_textures();
    }
    if (g_backend) ImGui_ImplVulkan_Shutdown();
    g_backend = false;
    if (g_device) destroy_composite();
    if (g_render_pass) f.destroy_render_pass(g_device, g_render_pass, nullptr);
    g_render_pass = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_format = VK_FORMAT_UNDEFINED;
    renderer_ready(false);
}

bool can_draw(uint32_t family) {
    return family < g_family_flags.size() && (g_family_flags[family] & VK_QUEUE_GRAPHICS_BIT);
}

// The queue to draw this present on: its own when it can (the game holds it for the present's length), else the game's
// graphics queue winmm.dll found (submitted to under its lock_queue). False: neither can draw.
bool pick_queue(const g4a_vk_present& p, VkQueue& queue, uint32_t& family) {
    if (can_draw(p.queue_family)) {
        queue = VkQueue(p.queue);
        family = p.queue_family;
        return true;
    }
    if (!p.draw_queue || !p.lock_queue || !can_draw(p.draw_queue_family)) return false;
    queue = VkQueue(p.draw_queue);
    family = p.draw_queue_family;
    return true;
}

// Sdr: one subpass on the swapchain's image. HDR: subpass 0 draws ImGui into the panel attachment (1), cleared to
// transparent black; subpass 1 composes it over the image (0), reading it as an input attachment (and for Hdr10 the image
// too, in GENERAL layout: one full-screen triangle reads and writes each pixel once).
VkRenderPass make_render_pass(Mode mode) {
    VkAttachmentDescription attachments[2] = {};
    VkAttachmentDescription& color = attachments[0];
    color.format = g_format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // the game's frame stays
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription& panel = attachments[1];
    panel.format = VK_FORMAT_R8G8B8A8_UNORM;
    panel.samples = VK_SAMPLE_COUNT_1_BIT;
    panel.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    panel.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    panel.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    panel.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    panel.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    panel.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const VkImageLayout image_layout = mode == Mode::Hdr10 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference image_ref{0, image_layout};
    const VkAttachmentReference panel_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference inputs[2] = {{1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {0, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription subpasses[2] = {};
    subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[0].colorAttachmentCount = 1;
    subpasses[0].pColorAttachments = mode == Mode::Sdr ? &image_ref : &panel_ref;
    subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpasses[1].colorAttachmentCount = 1;
    subpasses[1].pColorAttachments = &image_ref;
    subpasses[1].inputAttachmentCount = mode == Mode::Hdr10 ? 2 : 1;
    subpasses[1].pInputAttachments = inputs;

    VkSubpassDependency deps[3] = {};
    // The game's image: its writes (the present's semaphores, waited at these stages) before the panel's.
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = mode == Mode::Sdr ? 0 : 1;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (mode == Mode::Hdr10) {
        deps[0].dstStageMask |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstAccessMask |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    }
    // The panel attachment: cleared and drawn in subpass 0, read by subpass 1 at the same pixel.
    deps[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].dstSubpass = 0;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[2].srcSubpass = 0;
    deps[2].dstSubpass = 1;
    deps[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[2].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    deps[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = mode == Mode::Sdr ? 1 : 2;
    rp.pAttachments = attachments;
    rp.subpassCount = mode == Mode::Sdr ? 1 : 2;
    rp.pSubpasses = subpasses;
    rp.dependencyCount = mode == Mode::Sdr ? 1 : 3;
    rp.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    return f.create_render_pass(g_device, &rp, nullptr, &pass) == VK_SUCCESS ? pass : VK_NULL_HANDLE;
}

VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    return f.create_shader_module(g_device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

// Subpass 1's pipeline: a full-screen triangle, the mode's fragment shader, its input attachments in one set and the
// white level as a push constant. ScRgb and Hdr10Pq blend premultiplied (ONE, ONE_MINUS_SRC_ALPHA); Hdr10 blends in the
// shader. Colour channels only: the image's alpha stays the game's.
bool make_composite(Mode mode) {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = mode == Mode::Hdr10 ? 2 : 1;
    set.pBindings = bindings;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &g_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (f.create_set_layout(g_device, &set, nullptr, &g_set_layout) != VK_SUCCESS ||
        f.create_pipeline_layout(g_device, &layout, nullptr, &g_pipeline_layout) != VK_SUCCESS)
        return false;

    const VkShaderModule vs = shader(kSpirv_vs, sizeof kSpirv_vs);
    const VkShaderModule ps = mode == Mode::ScRgb ? shader(kSpirv_ps_scrgb, sizeof kSpirv_ps_scrgb)
                              : mode == Mode::Hdr10 ? shader(kSpirv_ps_hdr10, sizeof kSpirv_ps_hdr10)
                                                    : shader(kSpirv_ps_hdr10_pq, sizeof kSpirv_ps_hdr10_pq);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "vs";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = mode == Mode::ScRgb ? "ps_scrgb" : mode == Mode::Hdr10 ? "ps_hdr10" : "ps_hdr10_pq";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = mode == Mode::Hdr10 ? VK_FALSE : VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &color_blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = g_pipeline_layout;
    pipeline.renderPass = g_render_pass;
    pipeline.subpass = 1;
    const bool ok = vs && ps &&
                    f.create_graphics_pipelines(g_device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &g_composite) == VK_SUCCESS;
    if (vs) f.destroy_shader_module(g_device, vs, nullptr);
    if (ps) f.destroy_shader_module(g_device, ps, nullptr);
    return ok;
}

bool init(const g4a_vk_present& p) {
    g_instance = static_cast<VkInstance>(p.instance);
    g_physical = static_cast<VkPhysicalDevice>(p.physical_device);
    g_device = static_cast<VkDevice>(p.device);
    g_format = VkFormat(p.format);
    f = {};
    f.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(p.get_instance_proc_addr);
    if (!f.get_instance_proc || !load_functions()) return false;
    f.memory_properties(g_physical, &g_memory);
    if (!vertex_memory_maps()) {
        log("Vulkan: ImGui's vertex buffers couldn't be mapped on this device: panel unavailable");
        return false;
    }

    uint32_t families = 0;
    f.queue_family_properties(g_physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    f.queue_family_properties(g_physical, &families, props.data());
    g_family_flags.clear();
    for (const VkQueueFamilyProperties& prop : props) g_family_flags.push_back(prop.queueFlags);
    if (!pick_queue(p, g_queue, g_family)) {
        log("Vulkan: the game presents on queue family %u, which can't draw, and has no graphics queue: panel "
            "unavailable", p.queue_family);
        return false;
    }
    g_present_family = p.queue_family;

    g_color_space = p.color_space;
    g_input_attachment = p.input_attachment != 0;
    g_mode = pick_mode(p.color_space, g_input_attachment);
    if (g_mode != Mode::Sdr) {
        g_render_pass = make_render_pass(g_mode);
        if (!g_render_pass || !make_composite(g_mode)) {
            log("Vulkan: HDR composition (%s) unavailable: the panel is drawn directly (wrong colours on this HDR "
                "swapchain)", name(g_mode));
            destroy_composite();
            if (g_render_pass) f.destroy_render_pass(g_device, g_render_pass, nullptr);
            g_mode = Mode::Sdr;
        }
    }
    if (g_mode == Mode::Sdr) g_render_pass = make_render_pass(Mode::Sdr);
    if (!g_render_pass) {
        log("Vulkan: no render pass for format %d: panel unavailable", int(g_format));
        return false;
    }

    if (!ImGui_ImplVulkan_LoadFunctions(p.api_version, &backend_loader)) {
        log("ImGui's Vulkan backend could not load its functions: panel unavailable");
        return false;
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = p.api_version;
    info.Instance = g_instance;
    info.PhysicalDevice = g_physical;
    info.Device = g_device;
    info.QueueFamily = g_family;
    info.Queue = g_queue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = 2;
    info.ImageCount = p.image_count > 2 ? p.image_count : 2;
    info.PipelineInfoMain.RenderPass = g_render_pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = &check;
    if (!ImGui_ImplVulkan_Init(&info)) {
        log("ImGui's Vulkan backend did not start: panel unavailable");
        return false;
    }
    g_backend = true;
    renderer_ready(true);
    log("Vulkan renderer ready on device %p (API %u.%u, queue family %u, format %d, color space %u: %s)", p.device,
        VK_API_VERSION_MAJOR(p.api_version), VK_API_VERSION_MINOR(p.api_version), g_family, int(g_format),
        p.color_space, name(g_mode));
    return true;
}

// HDR: the panel's SDR attachment, the swapchain's size, and a pool for the frames' descriptor sets.
bool make_panel_target(const g4a_vk_present& p) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {p.width, p.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (f.create_image(g_device, &image, nullptr, &g_panel_image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    f.image_requirements(g_device, g_panel_image, &req);
    const int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    if (type < 0 || f.allocate_memory(g_device, &alloc, nullptr, &g_panel_memory) != VK_SUCCESS ||
        f.bind_image_memory(g_device, g_panel_image, g_panel_memory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = g_panel_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (f.create_image_view(g_device, &view, nullptr, &g_panel_view) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2 * p.image_count};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = p.image_count;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    return f.create_descriptor_pool(g_device, &pool, nullptr, &g_descriptor_pool) == VK_SUCCESS;
}

// HDR: one image's descriptor set (the panel attachment; Hdr10: the image itself too).
bool make_frame_set(Frame& fr) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = g_descriptor_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &g_set_layout;
    if (f.allocate_descriptor_sets(g_device, &alloc, &fr.set) != VK_SUCCESS) return false;
    const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, g_panel_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {VK_NULL_HANDLE, fr.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].dstSet = fr.set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        writes[i].pImageInfo = &images[i];
    }
    f.update_descriptor_sets(g_device, g_mode == Mode::Hdr10 ? 2 : 1, writes, 0, nullptr);
    return true;
}

bool make_frames(const g4a_vk_present& p) {
    destroy_frames();
    if (g_mode != Mode::Sdr && !make_panel_target(p)) {
        log("Vulkan: could not create the panel's HDR attachment (%ux%u)", p.width, p.height);
        destroy_frames();
        return false;
    }
    g_frames.resize(p.image_count);
    for (uint32_t i = 0; i < p.image_count; ++i) {
        Frame& fr = g_frames[i];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = VkImage(p.images[i]);
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g_format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = g_family;
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (f.create_image_view(g_device, &view, nullptr, &fr.view) != VK_SUCCESS ||
            f.create_command_pool(g_device, &pool, nullptr, &fr.pool) != VK_SUCCESS ||
            f.create_fence(g_device, &fence, nullptr, &fr.fence) != VK_SUCCESS ||
            f.create_semaphore(g_device, &sem, nullptr, &fr.done) != VK_SUCCESS) {
            log("Vulkan: could not create the panel's objects for swapchain image %u", i);
            destroy_frames();
            return false;
        }
        const VkImageView views[2] = {fr.view, g_panel_view};
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = g_render_pass;
        fb.attachmentCount = g_mode == Mode::Sdr ? 1 : 2;
        fb.pAttachments = views;
        fb.width = p.width;
        fb.height = p.height;
        fb.layers = 1;
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = fr.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (f.create_framebuffer(g_device, &fb, nullptr, &fr.framebuffer) != VK_SUCCESS ||
            f.allocate_command_buffers(g_device, &alloc, &fr.cmd) != VK_SUCCESS ||
            (g_mode != Mode::Sdr && !make_frame_set(fr))) {
            log("Vulkan: could not create the panel's framebuffer for swapchain image %u", i);
            destroy_frames();
            return false;
        }
        if (g_present_family == g_family || p.concurrent) continue;
        VkCommandPoolCreateInfo present_pool = pool;
        present_pool.queueFamilyIndex = g_present_family;
        VkCommandBuffer cmds[2] = {};
        VkCommandBufferAllocateInfo present_alloc = alloc;
        present_alloc.commandBufferCount = 2;
        const bool pool_made = f.create_command_pool(g_device, &present_pool, nullptr, &fr.present_pool) == VK_SUCCESS;
        present_alloc.commandPool = fr.present_pool;
        if (!pool_made || f.allocate_command_buffers(g_device, &present_alloc, cmds) != VK_SUCCESS ||
            f.create_semaphore(g_device, &sem, nullptr, &fr.released) != VK_SUCCESS ||
            f.create_semaphore(g_device, &sem, nullptr, &fr.drawn) != VK_SUCCESS) {
            log("Vulkan: could not create the panel's queue transfers for swapchain image %u", i);
            destroy_frames();
            return false;
        }
        fr.release = cmds[0];
        fr.acquire = cmds[1];
    }
    g_frames_present_family = g_present_family;
    g_swapchain = p.swapchain;
    g_images.assign(p.images, p.images + p.image_count);
    g_width = p.width;
    g_height = p.height;
    ImGui_ImplVulkan_SetMinImageCount(2);
    log("Vulkan: panel targets on swapchain %llx (%u images, %ux%u)", static_cast<unsigned long long>(p.swapchain),
        p.image_count, p.width, p.height);
    return true;
}

// One side of a queue family ownership transfer of a swapchain image (release on `from`'s queue, acquire on `to`'s),
// its layout left as PRESENT_SRC_KHR.
void ownership(VkCommandBuffer cmd, VkImage image, uint32_t from, uint32_t to, VkPipelineStageFlags src_stage,
               VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = from;
    barrier.dstQueueFamilyIndex = to;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace

bool present(g4a_vk_present& p) {
    Lock lock;
    p.signal_semaphore = 0;
    if (!p.device || !p.image_count || p.image_index >= p.image_count) return false;
    if (p.device != g_device || VkFormat(p.format) != g_format || p.color_space != g_color_space ||
        (p.input_attachment != 0) != g_input_attachment) {
        if (g_device) {
            log("Vulkan: the game presents with another device, format or color space now: renderer restarted");
            shutdown(true);
        }
        if (g_failed) return false;
        if (!init(p)) {
            g_failed = true;
            shutdown(true);
            return false;
        }
    }
    // The game may present from another queue than at the start: DOOM 2016 on AMD presents from its graphics queue in
    // the front end and from its compute queue in game. A queue that can draw is drawn on (the game holds it for the
    // present's length); one that can't has the panel drawn on the game's graphics queue (pick_queue), with the image's
    // ownership handed over and back when it is exclusive. Another draw family gets frames of its own (command pools
    // are per family); with no queue to draw on the panel skips these presents, and with no present seen it closes
    // instead of capturing input unseen.
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    const bool drawable = pick_queue(p, queue, family);
    if (!drawable || queue != g_queue || family != g_family || p.queue_family != g_present_family) {
        static int logged = 0;
        if (logged < 5)
            log("Vulkan: the game presents from queue %p (family %u) now; %s%s", p.queue, p.queue_family,
                !drawable                      ? "that family can't draw and there's no graphics queue: no panel"
                : queue == VkQueue(p.queue)    ? "the panel draws there"
                : p.concurrent                 ? "the panel draws on the game's graphics queue (images shared)"
                                               : "the panel draws on the game's graphics queue, the image handed over",
                ++logged == 5 ? " (further changes not logged)" : "");
        if (!drawable) return false;
        if (p.queue_family != g_present_family) destroy_frames();  // made again below (transfers or not)
        if (family != g_family) {
            // Frames made again below for the new family; ImGui's textures (exclusive to the old family) uploaded
            // again before this present's draw, which reads their new descriptors. destroy_frames waited for every
            // panel submission, so nothing uses the old ones.
            destroy_frames();
            free_staging();
            for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
                if (tex->BackendUserData) {
                    destroy_texture(tex);
                    tex->SetStatus(ImTextureStatus_WantCreate);
                }
        }
        g_queue = queue;
        g_family = family;
        g_present_family = p.queue_family;
    }
    present_seen();
    backbuffer_size(p.width, p.height, true);
    if (p.swapchain != g_swapchain || p.width != g_width || p.height != g_height || g_frames.size() != p.image_count ||
        !std::equal(g_images.begin(), g_images.end(), p.images) || g_frames_present_family != g_present_family)
        if (!make_frames(p)) return false;

    ImDrawData* data = draw_data();
    if (!data) return true;  // panel closed: the present is left alone
    Frame& fr = g_frames[p.image_index];
    // Every earlier panel submission must be done: this image's command buffer and semaphore are reused, ImGui's
    // backend rotates its vertex buffers on a count of its own, and textures may be replaced below. Never waits longer
    // than 100 ms (that frame goes without the panel).
    std::vector<VkFence> fences;
    for (const Frame& other : g_frames)
        if (other.fence) fences.push_back(other.fence);
    if (fences.size() != g_frames.size() ||
        f.wait_for_fences(g_device, uint32_t(fences.size()), fences.data(), VK_TRUE, 100'000'000ull) != VK_SUCCESS) {
        static int skipped = 0;
        if (++skipped <= 3) log("Vulkan: the panel's last submissions not done after 100 ms: this frame goes without it");
        return true;
    }
    free_staging();
    f.reset_fences(g_device, 1, &fr.fence);
    f.reset_command_pool(g_device, fr.pool, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // Drawn on the game's graphics queue for a present from one that can't draw, with exclusive images: the present's
    // family releases the image to the draw family, which acquires it, draws and releases it back (fr.release, fr.cmd,
    // fr.acquire, chained by semaphores; the fence on the last). Layouts stay PRESENT_SRC_KHR.
    const VkImage image = VkImage(p.images[p.image_index]);
    const bool transfer = g_present_family != g_family && !p.concurrent && fr.release && fr.acquire;
    // Hdr10 reads the game's pixel in its fragment shader, an earlier stage than colour output.
    const VkPipelineStageFlags draw_stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (g_mode == Mode::Hdr10 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0);
    f.begin_command_buffer(fr.cmd, &begin);
    if (transfer)
        ownership(fr.cmd, image, g_present_family, g_family, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, draw_stages,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      (g_mode == Mode::Hdr10 ? VK_ACCESS_INPUT_ATTACHMENT_READ_BIT : 0));
    ImGui_ImplVulkan_NewFrame();
    // ImGui's texture requests, recorded here; the backend then leaves textures alone (no queue wait in the present).
    if (data->Textures)
        for (ImTextureData* tex : *data->Textures)
            if (tex->Status != ImTextureStatus_OK && !upload_texture(tex, fr.cmd)) {
                static bool logged = false;
                if (!logged) log("Vulkan: could not upload an ImGui texture (%dx%d)", tex->Width, tex->Height);
                logged = true;
            }
    data->Textures = nullptr;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_render_pass;
    rp.framebuffer = fr.framebuffer;
    rp.renderArea.extent = {p.width, p.height};
    const VkClearValue clear[2] = {};  // [1]: the HDR panel attachment, transparent black ([0] is loaded)
    rp.clearValueCount = g_mode == Mode::Sdr ? 0 : 2;
    rp.pClearValues = clear;
    f.cmd_begin_render_pass(fr.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(data, fr.cmd);
    if (g_mode != Mode::Sdr) {
        f.cmd_next_subpass(fr.cmd, VK_SUBPASS_CONTENTS_INLINE);
        f.cmd_bind_pipeline(fr.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_composite);
        const VkViewport viewport{0, 0, float(p.width), float(p.height), 0, 1};
        const VkRect2D scissor{{0, 0}, {p.width, p.height}};
        f.cmd_set_viewport(fr.cmd, 0, 1, &viewport);
        f.cmd_set_scissor(fr.cmd, 0, 1, &scissor);
        f.cmd_bind_descriptor_sets(fr.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline_layout, 0, 1, &fr.set, 0, nullptr);
        const float white = g_mode == Mode::ScRgb ? kWhiteNits / 80.0f : kWhiteNits / 10000.0f;
        f.cmd_push_constants(fr.cmd, g_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof white, &white);
        f.cmd_draw(fr.cmd, 3, 1, 0, 0);
    }
    f.cmd_end_render_pass(fr.cmd);
    if (transfer)
        ownership(fr.cmd, image, g_family, g_present_family, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    f.end_command_buffer(fr.cmd);

    // The game's semaphores are waited on by the first submission; on a queue that can't draw, at every stage it has.
    const bool own_queue = g_queue == VkQueue(p.queue);
    std::vector<VkSemaphore> waits(p.wait_semaphore_count);
    std::vector<VkPipelineStageFlags> stages(
        p.wait_semaphore_count, own_queue || !transfer ? draw_stages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    for (uint32_t i = 0; i < p.wait_semaphore_count; ++i) waits[i] = VkSemaphore(p.wait_semaphores[i]);
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (transfer) {
        f.reset_command_pool(g_device, fr.present_pool, 0);
        f.begin_command_buffer(fr.release, &begin);
        ownership(fr.release, image, g_present_family, g_family, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        f.end_command_buffer(fr.release);
        f.begin_command_buffer(fr.acquire, &begin);
        ownership(fr.acquire, image, g_family, g_present_family, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        f.end_command_buffer(fr.acquire);
    }

    VkSubmitInfo submits[3] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO},
                               {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
    VkSubmitInfo& release = submits[0];  // present queue: the game's semaphores, image to the draw family
    release.waitSemaphoreCount = p.wait_semaphore_count;
    release.pWaitSemaphores = waits.data();
    release.pWaitDstStageMask = stages.data();
    release.commandBufferCount = 1;
    release.pCommandBuffers = &fr.release;
    release.signalSemaphoreCount = 1;
    release.pSignalSemaphores = &fr.released;
    VkSubmitInfo& draw = submits[1];  // draw queue
    if (transfer) {
        draw.waitSemaphoreCount = 1;
        draw.pWaitSemaphores = &fr.released;
        draw.pWaitDstStageMask = &draw_stages;
    } else {
        draw.waitSemaphoreCount = p.wait_semaphore_count;
        draw.pWaitSemaphores = waits.data();
        draw.pWaitDstStageMask = stages.data();
    }
    draw.commandBufferCount = 1;
    draw.pCommandBuffers = &fr.cmd;
    draw.signalSemaphoreCount = 1;
    draw.pSignalSemaphores = transfer ? &fr.drawn : &fr.done;
    VkSubmitInfo& acquire = submits[2];  // present queue: the image back, then the present
    acquire.waitSemaphoreCount = 1;
    acquire.pWaitSemaphores = &fr.drawn;
    acquire.pWaitDstStageMask = &all;
    acquire.commandBufferCount = 1;
    acquire.pCommandBuffers = &fr.acquire;
    acquire.signalSemaphoreCount = 1;
    acquire.pSignalSemaphores = &fr.done;

    // Another queue than the present's: the game may submit there from another thread (winmm.dll holds the same lock).
    const auto submit_draw = [&](VkFence fence) {
        if (!own_queue) p.lock_queue(g_queue, 1);
        const VkResult r = f.queue_submit(g_queue, 1, &draw, fence);
        if (!own_queue) p.lock_queue(g_queue, 0);
        return r;
    };
    VkResult r = VK_SUCCESS;
    const char* failed = nullptr;
    if (!transfer) {
        if ((r = submit_draw(fr.fence)) != VK_SUCCESS) failed = "draw";
    } else if ((r = f.queue_submit(VkQueue(p.queue), 1, &release, VK_NULL_HANDLE)) != VK_SUCCESS) {
        failed = "hand-over";
    } else if ((r = submit_draw(VK_NULL_HANDLE)) != VK_SUCCESS) {
        // The game's semaphores were waited on: the present waits on the hand-over instead (drawn without the panel).
        failed = "draw";
        p.signal_semaphore = uint64_t(fr.released);
    } else if ((r = f.queue_submit(VkQueue(p.queue), 1, &acquire, fr.fence)) != VK_SUCCESS) {
        failed = "hand-back";
        p.signal_semaphore = uint64_t(fr.drawn);
    }
    if (failed) {
        static bool logged = false;
        if (!logged) log("Vulkan: the panel's %s submission failed (%d): panel not drawn", failed, int(r));
        logged = true;
        // The fence was reset and nothing will signal it: recreate it signalled for the next use.
        f.destroy_fence(g_device, fr.fence, nullptr);
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (f.create_fence(g_device, &fence, nullptr, &fr.fence) != VK_SUCCESS) fr.fence = VK_NULL_HANDLE;
        return true;
    }
    p.signal_semaphore = uint64_t(fr.done);
    return true;
}

void swapchain_destroyed(uint64_t swapchain) {
    Lock lock;
    if (swapchain && swapchain == g_swapchain) destroy_frames();
}

void device_destroyed(void* device) {
    Lock lock;
    if (device && device == static_cast<void*>(g_device)) {
        shutdown();
        g_failed = false;  // a new device may work
    }
}

}  // namespace g4a::vk
