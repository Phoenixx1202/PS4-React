// Vulkan/RADV presentation for the PS4 runtime.
//
// Two 1920x1080 R8G8B8A8 linear images are registered with sceVideoOut and
// alternate as scanout buffers. Each present uploads the engine framebuffer
// into a B8G8R8A8 texture and draws a full-screen triangle that copies it into
// the current scanout image. Video decoding will add Y/UV planes to the same
// pipeline.

#include "vulkan_display.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "system.hpp"
#include "shared/video_view.hpp"
#include "vulkan_ui_shaders.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ps4 {
namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kBufferCount = 2;
constexpr VkDeviceSize kScanoutAlignment = 0x200000;
constexpr VkDeviceSize kFrameBytes =
    static_cast<VkDeviceSize>(kWidth) * kHeight * sizeof(std::uint32_t);
// The engine framebuffer is 0xAARRGGBB, so its bytes are B, G, R, A.
constexpr VkFormat kFrameFormat = VK_FORMAT_B8G8R8A8_UNORM;
// VideoOut reads the scanout image as A8B8G8R8; the R8G8B8A8 image stores the
// same bytes in the same order.
constexpr VkFormat kScanoutFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t kVideoOutFormat = ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB;

// Must match the push_constant block in shaders/ui.frag.
struct VideoPush {
    float rect[4];
    float draw[4];
    float flags[4];
};
static_assert(sizeof(VideoPush) == 48, "video push constants");

extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char* name);

PFN_vkGetInstanceProcAddr g_get_instance_proc = nullptr;
PFN_vkGetDeviceProcAddr g_get_device_proc = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
int g_queue_family = -1;
int g_video_out = -1;
OrbisKernelEqueue g_flip_queue = 0;
unsigned g_frame = 0;
int g_buffer_index = 0;

VkImage g_scanout[kBufferCount] = {};
VkDeviceMemory g_scanout_memory[kBufferCount] = {};
VkImageView g_scanout_view[kBufferCount] = {};
VkFramebuffer g_framebuffer[kBufferCount] = {};
void* g_scanout_mapped[kBufferCount] = {};
void* g_scanout_address[kBufferCount] = {};
std::uint32_t g_scanout_pitch = 0;

VkBuffer g_staging = VK_NULL_HANDLE;
VkDeviceMemory g_staging_memory = VK_NULL_HANDLE;
void* g_staging_mapped = nullptr;

VkImage g_video_y = VK_NULL_HANDLE;
VkImage g_video_uv = VK_NULL_HANDLE;
VkDeviceMemory g_video_y_memory = VK_NULL_HANDLE;
VkDeviceMemory g_video_uv_memory = VK_NULL_HANDLE;
VkImageView g_video_y_view = VK_NULL_HANDLE;
VkImageView g_video_uv_view = VK_NULL_HANDLE;
VkImageLayout g_video_y_layout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_video_uv_layout = VK_IMAGE_LAYOUT_UNDEFINED;
int g_video_width = 0;
int g_video_height = 0;
std::uint64_t g_video_serial = 0;
VkBuffer g_video_staging = VK_NULL_HANDLE;
VkDeviceMemory g_video_staging_memory = VK_NULL_HANDLE;
void* g_video_staging_mapped = nullptr;
VkDeviceSize g_video_staging_size = 0;

VkImage g_texture = VK_NULL_HANDLE;
VkDeviceMemory g_texture_memory = VK_NULL_HANDLE;
VkImageView g_texture_view = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
VkImageLayout g_texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;

VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet g_descriptor_set = VK_NULL_HANDLE;
VkShaderModule g_vertex_shader = VK_NULL_HANDLE;
VkShaderModule g_fragment_shader = VK_NULL_HANDLE;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkRenderPass g_render_pass = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;

VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_command = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

PFN_vkGetPhysicalDeviceMemoryProperties g_get_memory_properties = nullptr;
PFN_vkGetPhysicalDeviceFormatProperties g_get_format_properties = nullptr;
PFN_vkGetDeviceQueue g_get_queue = nullptr;
PFN_vkCreateImage g_create_image = nullptr;
PFN_vkDestroyImage g_destroy_image = nullptr;
PFN_vkGetImageMemoryRequirements g_get_image_requirements = nullptr;
PFN_vkGetImageSubresourceLayout g_get_image_layout = nullptr;
PFN_vkCreateImageView g_create_image_view = nullptr;
PFN_vkDestroyImageView g_destroy_image_view = nullptr;
PFN_vkCreateBuffer g_create_buffer = nullptr;
PFN_vkDestroyBuffer g_destroy_buffer = nullptr;
PFN_vkGetBufferMemoryRequirements g_get_buffer_requirements = nullptr;
PFN_vkBindBufferMemory g_bind_buffer_memory = nullptr;
PFN_vkAllocateMemory g_allocate_memory = nullptr;
PFN_vkFreeMemory g_free_memory = nullptr;
PFN_vkMapMemory g_map_memory = nullptr;
PFN_vkUnmapMemory g_unmap_memory = nullptr;
PFN_vkBindImageMemory g_bind_image_memory = nullptr;
PFN_vkCreateSampler g_create_sampler = nullptr;
PFN_vkDestroySampler g_destroy_sampler = nullptr;
PFN_vkCreateDescriptorSetLayout g_create_set_layout = nullptr;
PFN_vkDestroyDescriptorSetLayout g_destroy_set_layout = nullptr;
PFN_vkCreateDescriptorPool g_create_descriptor_pool = nullptr;
PFN_vkDestroyDescriptorPool g_destroy_descriptor_pool = nullptr;
PFN_vkAllocateDescriptorSets g_allocate_descriptor_sets = nullptr;
PFN_vkUpdateDescriptorSets g_update_descriptor_sets = nullptr;
PFN_vkCreateShaderModule g_create_shader_module = nullptr;
PFN_vkDestroyShaderModule g_destroy_shader_module = nullptr;
PFN_vkCreatePipelineLayout g_create_pipeline_layout = nullptr;
PFN_vkDestroyPipelineLayout g_destroy_pipeline_layout = nullptr;
PFN_vkCreateRenderPass g_create_render_pass = nullptr;
PFN_vkDestroyRenderPass g_destroy_render_pass = nullptr;
PFN_vkCreateGraphicsPipelines g_create_graphics_pipelines = nullptr;
PFN_vkDestroyPipeline g_destroy_pipeline = nullptr;
PFN_vkCreateFramebuffer g_create_framebuffer = nullptr;
PFN_vkDestroyFramebuffer g_destroy_framebuffer = nullptr;
PFN_vkCreateCommandPool g_create_command_pool = nullptr;
PFN_vkDestroyCommandPool g_destroy_command_pool = nullptr;
PFN_vkAllocateCommandBuffers g_allocate_command_buffers = nullptr;
PFN_vkBeginCommandBuffer g_begin_command_buffer = nullptr;
PFN_vkEndCommandBuffer g_end_command_buffer = nullptr;
PFN_vkCmdPipelineBarrier g_cmd_pipeline_barrier = nullptr;
PFN_vkCmdCopyBufferToImage g_cmd_copy_buffer_to_image = nullptr;
PFN_vkCmdBeginRenderPass g_cmd_begin_render_pass = nullptr;
PFN_vkCmdEndRenderPass g_cmd_end_render_pass = nullptr;
PFN_vkCmdBindPipeline g_cmd_bind_pipeline = nullptr;
PFN_vkCmdBindDescriptorSets g_cmd_bind_descriptor_sets = nullptr;
PFN_vkCmdPushConstants g_cmd_push_constants = nullptr;
PFN_vkCmdDraw g_cmd_draw = nullptr;
PFN_vkCreateFence g_create_fence = nullptr;
PFN_vkDestroyFence g_destroy_fence = nullptr;
PFN_vkResetFences g_reset_fences = nullptr;
PFN_vkWaitForFences g_wait_for_fences = nullptr;
PFN_vkQueueSubmit g_queue_submit = nullptr;
PFN_vkDeviceWaitIdle g_device_wait_idle = nullptr;
PFN_vkDestroyDevice g_destroy_device = nullptr;
PFN_vkDestroyInstance g_destroy_instance = nullptr;

PFN_vkVoidFunction instance_fn(VkInstance instance, const char* name) {
    return g_get_instance_proc ? g_get_instance_proc(instance, name) : nullptr;
}

PFN_vkVoidFunction device_fn(VkDevice device, const char* name) {
    return g_get_device_proc ? g_get_device_proc(device, name) : nullptr;
}

bool load_device_functions() {
    const auto load = [](const char* name) { return device_fn(g_device, name); };
    g_get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(load("vkGetDeviceQueue"));
    g_create_image = reinterpret_cast<PFN_vkCreateImage>(load("vkCreateImage"));
    g_destroy_image = reinterpret_cast<PFN_vkDestroyImage>(load("vkDestroyImage"));
    g_get_image_requirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        load("vkGetImageMemoryRequirements"));
    g_get_image_layout = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
        load("vkGetImageSubresourceLayout"));
    g_create_image_view = reinterpret_cast<PFN_vkCreateImageView>(load("vkCreateImageView"));
    g_destroy_image_view =
        reinterpret_cast<PFN_vkDestroyImageView>(load("vkDestroyImageView"));
    g_create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(load("vkCreateBuffer"));
    g_destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(load("vkDestroyBuffer"));
    g_get_buffer_requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
        load("vkGetBufferMemoryRequirements"));
    g_bind_buffer_memory =
        reinterpret_cast<PFN_vkBindBufferMemory>(load("vkBindBufferMemory"));
    g_allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(load("vkAllocateMemory"));
    g_free_memory = reinterpret_cast<PFN_vkFreeMemory>(load("vkFreeMemory"));
    g_map_memory = reinterpret_cast<PFN_vkMapMemory>(load("vkMapMemory"));
    g_unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(load("vkUnmapMemory"));
    g_bind_image_memory =
        reinterpret_cast<PFN_vkBindImageMemory>(load("vkBindImageMemory"));
    g_create_sampler = reinterpret_cast<PFN_vkCreateSampler>(load("vkCreateSampler"));
    g_destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(load("vkDestroySampler"));
    g_create_set_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
        load("vkCreateDescriptorSetLayout"));
    g_destroy_set_layout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
        load("vkDestroyDescriptorSetLayout"));
    g_create_descriptor_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
        load("vkCreateDescriptorPool"));
    g_destroy_descriptor_pool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
        load("vkDestroyDescriptorPool"));
    g_allocate_descriptor_sets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
        load("vkAllocateDescriptorSets"));
    g_update_descriptor_sets =
        reinterpret_cast<PFN_vkUpdateDescriptorSets>(load("vkUpdateDescriptorSets"));
    g_create_shader_module =
        reinterpret_cast<PFN_vkCreateShaderModule>(load("vkCreateShaderModule"));
    g_destroy_shader_module =
        reinterpret_cast<PFN_vkDestroyShaderModule>(load("vkDestroyShaderModule"));
    g_create_pipeline_layout =
        reinterpret_cast<PFN_vkCreatePipelineLayout>(load("vkCreatePipelineLayout"));
    g_destroy_pipeline_layout =
        reinterpret_cast<PFN_vkDestroyPipelineLayout>(load("vkDestroyPipelineLayout"));
    g_create_render_pass =
        reinterpret_cast<PFN_vkCreateRenderPass>(load("vkCreateRenderPass"));
    g_destroy_render_pass =
        reinterpret_cast<PFN_vkDestroyRenderPass>(load("vkDestroyRenderPass"));
    g_create_graphics_pipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
        load("vkCreateGraphicsPipelines"));
    g_destroy_pipeline =
        reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
    g_create_framebuffer =
        reinterpret_cast<PFN_vkCreateFramebuffer>(load("vkCreateFramebuffer"));
    g_destroy_framebuffer =
        reinterpret_cast<PFN_vkDestroyFramebuffer>(load("vkDestroyFramebuffer"));
    g_create_command_pool =
        reinterpret_cast<PFN_vkCreateCommandPool>(load("vkCreateCommandPool"));
    g_destroy_command_pool =
        reinterpret_cast<PFN_vkDestroyCommandPool>(load("vkDestroyCommandPool"));
    g_allocate_command_buffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        load("vkAllocateCommandBuffers"));
    g_begin_command_buffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(load("vkBeginCommandBuffer"));
    g_end_command_buffer =
        reinterpret_cast<PFN_vkEndCommandBuffer>(load("vkEndCommandBuffer"));
    g_cmd_pipeline_barrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(load("vkCmdPipelineBarrier"));
    g_cmd_copy_buffer_to_image = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
        load("vkCmdCopyBufferToImage"));
    g_cmd_begin_render_pass =
        reinterpret_cast<PFN_vkCmdBeginRenderPass>(load("vkCmdBeginRenderPass"));
    g_cmd_end_render_pass =
        reinterpret_cast<PFN_vkCmdEndRenderPass>(load("vkCmdEndRenderPass"));
    g_cmd_bind_pipeline =
        reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
    g_cmd_bind_descriptor_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
        load("vkCmdBindDescriptorSets"));
    g_cmd_push_constants =
        reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
    g_cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(load("vkCmdDraw"));
    g_create_fence = reinterpret_cast<PFN_vkCreateFence>(load("vkCreateFence"));
    g_destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(load("vkDestroyFence"));
    g_reset_fences = reinterpret_cast<PFN_vkResetFences>(load("vkResetFences"));
    g_wait_for_fences = reinterpret_cast<PFN_vkWaitForFences>(load("vkWaitForFences"));
    g_queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(load("vkQueueSubmit"));
    g_device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(load("vkDeviceWaitIdle"));
    g_destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(load("vkDestroyDevice"));
    return g_get_queue && g_create_image && g_destroy_image && g_get_image_requirements &&
           g_get_image_layout && g_create_image_view && g_destroy_image_view &&
           g_create_buffer && g_destroy_buffer && g_get_buffer_requirements &&
           g_bind_buffer_memory && g_allocate_memory && g_free_memory && g_map_memory &&
           g_unmap_memory && g_bind_image_memory && g_create_sampler && g_destroy_sampler &&
           g_create_set_layout && g_destroy_set_layout && g_create_descriptor_pool &&
           g_destroy_descriptor_pool && g_allocate_descriptor_sets &&
           g_update_descriptor_sets && g_create_shader_module && g_destroy_shader_module &&
           g_create_pipeline_layout && g_destroy_pipeline_layout && g_create_render_pass &&
           g_destroy_render_pass && g_create_graphics_pipelines && g_destroy_pipeline &&
           g_create_framebuffer && g_destroy_framebuffer && g_create_command_pool &&
           g_destroy_command_pool && g_allocate_command_buffers && g_begin_command_buffer &&
           g_end_command_buffer && g_cmd_pipeline_barrier && g_cmd_copy_buffer_to_image &&
           g_cmd_begin_render_pass && g_cmd_end_render_pass && g_cmd_bind_pipeline &&
           g_cmd_bind_descriptor_sets && g_cmd_push_constants && g_cmd_draw &&
           g_create_fence && g_destroy_fence && g_reset_fences && g_wait_for_fences &&
           g_queue_submit && g_device_wait_idle && g_destroy_device;
}

std::uint32_t pick_memory_type(std::uint32_t bits, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred = 0) {
    VkPhysicalDeviceMemoryProperties properties{};
    g_get_memory_properties(g_physical, &properties);
    std::uint32_t fallback = UINT32_MAX;
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) continue;
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

bool create_instance_and_device() {
    g_get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
    auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(instance_fn(nullptr, "vkCreateInstance"));
    if (!create_instance) return false;

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "PS4 React";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "PS4 React";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    VkResult result = create_instance(&instance_info, nullptr, &g_instance);
    system::log("vulkan: instance=0x%x %p", static_cast<unsigned>(result), g_instance);
    if (result != VK_SUCCESS || !g_instance) return false;

    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        instance_fn(g_instance, "vkEnumeratePhysicalDevices"));
    auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        instance_fn(g_instance, "vkGetPhysicalDeviceProperties"));
    auto get_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        instance_fn(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto create = reinterpret_cast<PFN_vkCreateDevice>(
        instance_fn(g_instance, "vkCreateDevice"));
    g_get_memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        instance_fn(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
    g_get_format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        instance_fn(g_instance, "vkGetPhysicalDeviceFormatProperties"));
    g_destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        instance_fn(g_instance, "vkDestroyInstance"));
    if (!enumerate || !get_properties || !get_families || !create ||
        !g_get_memory_properties || !g_get_format_properties || !g_destroy_instance)
        return false;

    std::uint32_t count = 0;
    if (enumerate(g_instance, &count, nullptr) != VK_SUCCESS || count == 0) return false;
    VkPhysicalDevice devices[4]{};
    if (count > 4) count = 4;
    if (enumerate(g_instance, &count, devices) != VK_SUCCESS) return false;
    g_physical = devices[0];
    VkPhysicalDeviceProperties properties{};
    get_properties(g_physical, &properties);
    system::log("vulkan: GPU='%s' api=%u.%u.%u", properties.deviceName,
                VK_VERSION_MAJOR(properties.apiVersion),
                VK_VERSION_MINOR(properties.apiVersion),
                VK_VERSION_PATCH(properties.apiVersion));

    VkFormatProperties frame_format{};
    g_get_format_properties(g_physical, kFrameFormat, &frame_format);
    VkFormatProperties scanout_format{};
    g_get_format_properties(g_physical, kScanoutFormat, &scanout_format);
    system::log("vulkan: formats frame=0x%x scanout=0x%x",
                static_cast<unsigned>(frame_format.optimalTilingFeatures),
                static_cast<unsigned>(scanout_format.linearTilingFeatures));
    if ((frame_format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0 ||
        (frame_format.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0 ||
        (scanout_format.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
        system::log("vulkan: required formats are not supported");
        return false;
    }

    std::uint32_t family_count = 0;
    get_families(g_physical, &family_count, nullptr);
    VkQueueFamilyProperties families[16]{};
    if (family_count > 16) family_count = 16;
    get_families(g_physical, &family_count, families);
    int family = -1;
    for (std::uint32_t i = 0; i < family_count; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && family < 0)
            family = static_cast<int>(i);
    if (family < 0) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = static_cast<std::uint32_t>(family);
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    result = create(g_physical, &device_info, nullptr, &g_device);
    system::log("vulkan: device=0x%x %p", static_cast<unsigned>(result), g_device);
    if (result != VK_SUCCESS || !g_device) return false;
    g_get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        instance_fn(g_instance, "vkGetDeviceProcAddr"));
    if (!g_get_device_proc) return false;
    g_queue_family = family;
    return load_device_functions();
}

bool create_scanout(int index) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kScanoutFormat;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_LINEAR;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = g_create_image(g_device, &image_info, nullptr, &g_scanout[index]);
    if (result != VK_SUCCESS || !g_scanout[index]) {
        system::log("vulkan: scanout %d image=0x%x", index, static_cast<unsigned>(result));
        return false;
    }

    VkMemoryRequirements requirements{};
    g_get_image_requirements(g_device, g_scanout[index], &requirements);
    const std::uint32_t type = pick_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        system::log("vulkan: scanout %d has no host-visible memory type", index);
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size + kScanoutAlignment + requirements.alignment;
    allocation.memoryTypeIndex = type;
    result = g_allocate_memory(g_device, &allocation, nullptr, &g_scanout_memory[index]);
    if (result != VK_SUCCESS || !g_scanout_memory[index]) {
        system::log("vulkan: scanout %d memory=0x%x", index, static_cast<unsigned>(result));
        return false;
    }
    if (g_map_memory(g_device, g_scanout_memory[index], 0, VK_WHOLE_SIZE, 0,
                     &g_scanout_mapped[index]) != VK_SUCCESS)
        return false;

    VkImageSubresource subresource{};
    subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkSubresourceLayout layout{};
    g_get_image_layout(g_device, g_scanout[index], &subresource, &layout);
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_scanout_mapped[index]);
    VkDeviceSize offset =
        (kScanoutAlignment - ((base + layout.offset) % kScanoutAlignment)) % kScanoutAlignment;
    if (requirements.alignment && offset % requirements.alignment)
        offset += requirements.alignment - offset % requirements.alignment;
    if (g_bind_image_memory(g_device, g_scanout[index], g_scanout_memory[index], offset) !=
        VK_SUCCESS)
        return false;

    g_scanout_address[index] =
        reinterpret_cast<void*>(base + offset + static_cast<std::uintptr_t>(layout.offset));
    const std::uint32_t pitch = static_cast<std::uint32_t>(layout.rowPitch / 4);
    if (g_scanout_pitch == 0)
        g_scanout_pitch = pitch;
    else if (g_scanout_pitch != pitch) {
        system::log("vulkan: scanout pitch mismatch %u != %u", pitch, g_scanout_pitch);
        return false;
    }
    system::log("vulkan: scanout %d address=%p pitch=%u offset=%lld size=%llu", index,
                g_scanout_address[index], pitch, static_cast<long long>(offset),
                static_cast<unsigned long long>(requirements.size));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = g_scanout[index];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kScanoutFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = g_create_image_view(g_device, &view_info, nullptr, &g_scanout_view[index]);
    if (result != VK_SUCCESS || !g_scanout_view[index]) {
        system::log("vulkan: scanout %d view=0x%x", index, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

bool create_texture() {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kFrameFormat;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = g_create_image(g_device, &image_info, nullptr, &g_texture);
    if (result != VK_SUCCESS || !g_texture) {
        system::log("vulkan: texture image=0x%x", static_cast<unsigned>(result));
        return false;
    }
    VkMemoryRequirements requirements{};
    g_get_image_requirements(g_device, g_texture, &requirements);
    const std::uint32_t type =
        pick_memory_type(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        system::log("vulkan: texture has no memory type");
        return false;
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = g_allocate_memory(g_device, &allocation, nullptr, &g_texture_memory);
    if (result != VK_SUCCESS || !g_texture_memory) {
        system::log("vulkan: texture memory=0x%x", static_cast<unsigned>(result));
        return false;
    }
    if (g_bind_image_memory(g_device, g_texture, g_texture_memory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = g_texture;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kFrameFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = g_create_image_view(g_device, &view_info, nullptr, &g_texture_view);
    if (result != VK_SUCCESS || !g_texture_view) {
        system::log("vulkan: texture view=0x%x", static_cast<unsigned>(result));
        return false;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    result = g_create_sampler(g_device, &sampler_info, nullptr, &g_sampler);
    if (result != VK_SUCCESS || !g_sampler) {
        system::log("vulkan: sampler=0x%x", static_cast<unsigned>(result));
        return false;
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = kFrameBytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = g_create_buffer(g_device, &buffer_info, nullptr, &g_staging);
    if (result != VK_SUCCESS || !g_staging) {
        system::log("vulkan: staging buffer=0x%x", static_cast<unsigned>(result));
        return false;
    }
    VkMemoryRequirements buffer_requirements{};
    g_get_buffer_requirements(g_device, g_staging, &buffer_requirements);
    const std::uint32_t staging_type = pick_memory_type(
        buffer_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging_type == UINT32_MAX) {
        system::log("vulkan: staging has no host-visible memory type");
        return false;
    }
    VkMemoryAllocateInfo staging_allocation{};
    staging_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    staging_allocation.allocationSize = buffer_requirements.size;
    staging_allocation.memoryTypeIndex = staging_type;
    result = g_allocate_memory(g_device, &staging_allocation, nullptr, &g_staging_memory);
    if (result != VK_SUCCESS || !g_staging_memory) {
        system::log("vulkan: staging memory=0x%x", static_cast<unsigned>(result));
        return false;
    }
    if (g_bind_buffer_memory(g_device, g_staging, g_staging_memory, 0) != VK_SUCCESS)
        return false;
    if (g_map_memory(g_device, g_staging_memory, 0, VK_WHOLE_SIZE, 0, &g_staging_mapped) !=
        VK_SUCCESS)
        return false;
    system::log("vulkan: frame texture %ux%u, staging %llu bytes", kWidth, kHeight,
                static_cast<unsigned long long>(kFrameBytes));
    return true;
}

bool create_video_texture(VkFormat format, int width, int height, VkImage* image,
                          VkDeviceMemory* memory, VkImageView* view) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = g_create_image(g_device, &image_info, nullptr, image);
    if (result != VK_SUCCESS || !*image) {
        system::log("vulkan: video image=0x%x %dx%d", static_cast<unsigned>(result), width, height);
        return false;
    }
    VkMemoryRequirements requirements{};
    g_get_image_requirements(g_device, *image, &requirements);
    const std::uint32_t type =
        pick_memory_type(requirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = g_allocate_memory(g_device, &allocation, nullptr, memory);
    if (result != VK_SUCCESS || !*memory) return false;
    if (g_bind_image_memory(g_device, *image, *memory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    result = g_create_image_view(g_device, &view_info, nullptr, view);
    return result == VK_SUCCESS && *view;
}

void destroy_video_textures() {
    if (g_device && g_destroy_image_view && g_video_y_view)
        g_destroy_image_view(g_device, g_video_y_view, nullptr);
    if (g_device && g_destroy_image_view && g_video_uv_view)
        g_destroy_image_view(g_device, g_video_uv_view, nullptr);
    if (g_device && g_destroy_image && g_video_y) g_destroy_image(g_device, g_video_y, nullptr);
    if (g_device && g_destroy_image && g_video_uv) g_destroy_image(g_device, g_video_uv, nullptr);
    if (g_device && g_free_memory && g_video_y_memory)
        g_free_memory(g_device, g_video_y_memory, nullptr);
    if (g_device && g_free_memory && g_video_uv_memory)
        g_free_memory(g_device, g_video_uv_memory, nullptr);
    g_video_y = VK_NULL_HANDLE;
    g_video_uv = VK_NULL_HANDLE;
    g_video_y_memory = VK_NULL_HANDLE;
    g_video_uv_memory = VK_NULL_HANDLE;
    g_video_y_view = VK_NULL_HANDLE;
    g_video_uv_view = VK_NULL_HANDLE;
    g_video_y_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_video_uv_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_video_width = 0;
    g_video_height = 0;
    g_video_serial = 0;
}

bool ensure_video_textures(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_video_y && g_video_uv && g_video_width == width && g_video_height == height)
        return true;
    destroy_video_textures();
    const int chroma_width = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    if (!create_video_texture(VK_FORMAT_R8_UNORM, width, height, &g_video_y,
                              &g_video_y_memory, &g_video_y_view) ||
        !create_video_texture(VK_FORMAT_R8G8_UNORM, chroma_width, chroma_height,
                              &g_video_uv, &g_video_uv_memory, &g_video_uv_view)) {
        destroy_video_textures();
        return false;
    }
    VkDescriptorImageInfo infos[2]{};
    infos[0].sampler = g_sampler;
    infos[0].imageView = g_video_y_view;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].sampler = g_sampler;
    infos[1].imageView = g_video_uv_view;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_descriptor_set;
        writes[i].dstBinding = i + 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    g_update_descriptor_sets(g_device, 2, writes, 0, nullptr);
    g_video_width = width;
    g_video_height = height;
    g_video_y_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_video_uv_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_video_serial = 0;
    system::log("vulkan: video textures %dx%d uv=%dx%d", width, height, chroma_width,
                chroma_height);
    return true;
}

void destroy_video_staging() {
    if (g_device && g_unmap_memory && g_video_staging_memory && g_video_staging_mapped)
        g_unmap_memory(g_device, g_video_staging_memory);
    if (g_device && g_destroy_buffer && g_video_staging)
        g_destroy_buffer(g_device, g_video_staging, nullptr);
    if (g_device && g_free_memory && g_video_staging_memory)
        g_free_memory(g_device, g_video_staging_memory, nullptr);
    g_video_staging = VK_NULL_HANDLE;
    g_video_staging_memory = VK_NULL_HANDLE;
    g_video_staging_mapped = nullptr;
    g_video_staging_size = 0;
}

bool ensure_video_staging(VkDeviceSize size) {
    if (size == 0) return false;
    if (g_video_staging_mapped && g_video_staging_size >= size) return true;
    destroy_video_staging();
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = g_create_buffer(g_device, &buffer_info, nullptr, &g_video_staging);
    if (result != VK_SUCCESS || !g_video_staging) return false;
    VkMemoryRequirements requirements{};
    g_get_buffer_requirements(g_device, g_video_staging, &requirements);
    const std::uint32_t type = pick_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    result = g_allocate_memory(g_device, &allocation, nullptr, &g_video_staging_memory);
    if (result != VK_SUCCESS || !g_video_staging_memory) return false;
    if (g_bind_buffer_memory(g_device, g_video_staging, g_video_staging_memory, 0) != VK_SUCCESS)
        return false;
    if (g_map_memory(g_device, g_video_staging_memory, 0, VK_WHOLE_SIZE, 0,
                     &g_video_staging_mapped) != VK_SUCCESS)
        return false;
    g_video_staging_size = requirements.size;
    system::log("vulkan: video staging %llu bytes",
                static_cast<unsigned long long>(g_video_staging_size));
    return true;
}

bool create_descriptors() {
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    VkResult result =
        g_create_set_layout(g_device, &layout_info, nullptr, &g_set_layout);
    if (result != VK_SUCCESS || !g_set_layout) return false;

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 3;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    result = g_create_descriptor_pool(g_device, &pool_info, nullptr, &g_descriptor_pool);
    if (result != VK_SUCCESS || !g_descriptor_pool) return false;

    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = g_descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &g_set_layout;
    result = g_allocate_descriptor_sets(g_device, &allocate_info, &g_descriptor_set);
    if (result != VK_SUCCESS || !g_descriptor_set) return false;

    // Until a video frame arrives, all three samplers point at the UI texture.
    VkDescriptorImageInfo image_info{};
    image_info.sampler = g_sampler;
    image_info.imageView = g_texture_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_info;
    }
    g_update_descriptor_sets(g_device, 3, writes, 0, nullptr);
    return true;
}

bool create_shaders() {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = kUiVertSpvSize;
    info.pCode = kUiVertSpv;
    VkResult result = g_create_shader_module(g_device, &info, nullptr, &g_vertex_shader);
    if (result != VK_SUCCESS || !g_vertex_shader) {
        system::log("vulkan: vertex shader=0x%x", static_cast<unsigned>(result));
        return false;
    }
    info.codeSize = kUiFragSpvSize;
    info.pCode = kUiFragSpv;
    result = g_create_shader_module(g_device, &info, nullptr, &g_fragment_shader);
    if (result != VK_SUCCESS || !g_fragment_shader) {
        system::log("vulkan: fragment shader=0x%x", static_cast<unsigned>(result));
        return false;
    }
    return true;
}

bool create_pipeline() {
    VkAttachmentDescription attachment{};
    attachment.format = kScanoutFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference color_reference{};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo render_info{};
    render_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_info.attachmentCount = 1;
    render_info.pAttachments = &attachment;
    render_info.subpassCount = 1;
    render_info.pSubpasses = &subpass;
    render_info.dependencyCount = 1;
    render_info.pDependencies = &dependency;
    VkResult result =
        g_create_render_pass(g_device, &render_info, nullptr, &g_render_pass);
    if (result != VK_SUCCESS || !g_render_pass) {
        system::log("vulkan: render pass=0x%x", static_cast<unsigned>(result));
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &g_set_layout;
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(VideoPush);
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = g_create_pipeline_layout(g_device, &layout_info, nullptr, &g_pipeline_layout);
    if (result != VK_SUCCESS || !g_pipeline_layout) {
        system::log("vulkan: pipeline layout=0x%x", static_cast<unsigned>(result));
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_fragment_shader;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{};
    viewport.width = static_cast<float>(kWidth);
    viewport.height = static_cast<float>(kHeight);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {kWidth, kHeight};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = g_pipeline_layout;
    pipeline_info.renderPass = g_render_pass;
    pipeline_info.subpass = 0;
    result = g_create_graphics_pipelines(g_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &g_pipeline);
    if (result != VK_SUCCESS || !g_pipeline) {
        system::log("vulkan: pipeline=0x%x", static_cast<unsigned>(result));
        return false;
    }

    for (int i = 0; i < kBufferCount; ++i) {
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = g_render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &g_scanout_view[i];
        framebuffer_info.width = kWidth;
        framebuffer_info.height = kHeight;
        framebuffer_info.layers = 1;
        result = g_create_framebuffer(g_device, &framebuffer_info, nullptr, &g_framebuffer[i]);
        if (result != VK_SUCCESS || !g_framebuffer[i]) {
            system::log("vulkan: framebuffer %d=0x%x", i, static_cast<unsigned>(result));
            return false;
        }
    }
    return true;
}

bool create_video_out() {
    using LoadInternal = std::uint32_t (*)(std::uint32_t);
    const auto load = reinterpret_cast<LoadInternal>(sceSysmoduleLoadModuleInternal);
    load(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    load(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    load(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    OrbisUserServiceInitializeParams user_params{};
    user_params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&user_params);

    g_video_out = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    system::log("vulkan: videoout=%d", g_video_out);
    if (g_video_out < 0) return false;
    if (sceKernelCreateEqueue(&g_flip_queue, "vulkan display") != 0) {
        system::log("vulkan: flip equeue failed");
        return false;
    }
    sceVideoOutAddFlipEvent(g_flip_queue, g_video_out, nullptr);

    OrbisVideoOutBufferAttribute attribute{};
    sceVideoOutSetBufferAttribute(&attribute, kVideoOutFormat, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  0, kWidth, kHeight, g_scanout_pitch);
    const int set = sceVideoOutRegisterBuffers(g_video_out, 0, g_scanout_address, kBufferCount,
                                               &attribute);
    system::log("vulkan: register=%d pitch=%u", set, g_scanout_pitch);
    if (set < 0) return false;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = static_cast<std::uint32_t>(g_queue_family);
    if (g_create_command_pool(g_device, &pool_info, nullptr, &g_pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = g_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (g_allocate_command_buffers(g_device, &command_info, &g_command) != VK_SUCCESS) return false;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g_create_fence(g_device, &fence_info, nullptr, &g_fence) != VK_SUCCESS) return false;
    g_get_queue(g_device, static_cast<std::uint32_t>(g_queue_family), 0, &g_queue);
    return g_queue != VK_NULL_HANDLE;
}

bool create_target() {
    for (int i = 0; i < kBufferCount; ++i)
        if (!create_scanout(i)) return false;
    if (!create_texture()) return false;
    if (!create_descriptors()) return false;
    if (!create_shaders()) return false;
    if (!create_pipeline()) return false;
    return create_video_out();
}

} // namespace

bool vulkan_display_open(int width, int height) {
    (void)width;
    (void)height;
    if (g_device) return true;
    if (!create_instance_and_device()) {
        system::log("vulkan: device creation failed");
        vulkan_display_close();
        return false;
    }
    if (!create_target()) {
        system::log("vulkan: target creation failed");
        vulkan_display_close();
        return false;
    }
    system::log("vulkan: display ready");
    return true;
}

bool vulkan_display_present(const std::uint32_t* pixels) {
    if (!pixels || !g_device || !g_command || !g_staging_mapped) return false;
    static int failures = 0;
    const auto fail = [&](const char* stage, int code) {
        if (failures++ < 8)
            system::log("vulkan: present %s=0x%x", stage, static_cast<unsigned>(code));
        return false;
    };

    std::memcpy(g_staging_mapped, pixels, static_cast<std::size_t>(kFrameBytes));
    g_reset_fences(g_device, 1, &g_fence);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begin_result = g_begin_command_buffer(g_command, &begin);
    if (begin_result != VK_SUCCESS) return fail("begin", begin_result);

    VkBufferMemoryBarrier buffer_barrier{};
    buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = g_staging;
    buffer_barrier.offset = 0;
    buffer_barrier.size = VK_WHOLE_SIZE;

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = g_texture;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.oldLayout = g_texture_layout;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcAccessMask =
        g_texture_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags upload_source =
        VK_PIPELINE_STAGE_HOST_BIT |
        (g_texture_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
             ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    g_cmd_pipeline_barrier(g_command, upload_source, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 1, &buffer_barrier, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    g_cmd_copy_buffer_to_image(g_command, g_staging, g_texture,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader{};
    to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.image = g_texture;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    g_cmd_pipeline_barrier(g_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_shader);
    g_texture_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ps4::VideoFrameView video{};
    bool video_active = ps4_react_video_frame_view(&video) && video.source_width > 0 &&
                        video.source_height > 0 && video.pitch > 0;
    if (video_active) {
        const int crop_left = std::clamp(video.crop_left, 0, video.source_width - 1);
        const int crop_top = std::clamp(video.crop_top, 0, video.source_height - 1);
        const int visible_width = std::min(video.visible_width, video.source_width - crop_left);
        const int visible_height = std::min(video.visible_height, video.source_height - crop_top);
        video_active = visible_width > 0 && visible_height > 0 && video.pitch >= visible_width &&
                       ensure_video_textures(visible_width, visible_height);
        if (video_active) {
            const VkDeviceSize video_bytes = static_cast<VkDeviceSize>(video.pitch) *
                                             video.source_height * 3 / 2;
            video_active = ensure_video_staging(video_bytes);
            if (video_active && video.serial != g_video_serial) {
                std::memcpy(g_video_staging_mapped, video.data,
                            static_cast<std::size_t>(video_bytes));
                VkBufferMemoryBarrier video_buffer{};
                video_buffer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                video_buffer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
                video_buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                video_buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_buffer.buffer = g_video_staging;
                video_buffer.offset = 0;
                video_buffer.size = VK_WHOLE_SIZE;

                VkImageMemoryBarrier video_to_transfer[2]{};
                video_to_transfer[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                video_to_transfer[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_to_transfer[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_to_transfer[0].image = g_video_y;
                video_to_transfer[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                video_to_transfer[0].oldLayout = g_video_y_layout;
                video_to_transfer[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                video_to_transfer[0].srcAccessMask =
                    g_video_y_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        ? VK_ACCESS_SHADER_READ_BIT
                        : 0;
                video_to_transfer[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                video_to_transfer[1] = video_to_transfer[0];
                video_to_transfer[1].image = g_video_uv;
                video_to_transfer[1].oldLayout = g_video_uv_layout;
                video_to_transfer[1].srcAccessMask =
                    g_video_uv_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        ? VK_ACCESS_SHADER_READ_BIT
                        : 0;
                const VkPipelineStageFlags video_source =
                    VK_PIPELINE_STAGE_HOST_BIT |
                    ((g_video_y_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
                      g_video_uv_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                         ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
                g_cmd_pipeline_barrier(g_command, video_source, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, 0, nullptr, 1, &video_buffer, 2,
                                       video_to_transfer);

                const int chroma_width = (visible_width + 1) / 2;
                const int chroma_height = (visible_height + 1) / 2;
                VkBufferImageCopy copies[2]{};
                copies[0].bufferOffset = static_cast<VkDeviceSize>(crop_top) * video.pitch +
                                         static_cast<VkDeviceSize>(crop_left);
                copies[0].bufferRowLength = static_cast<std::uint32_t>(video.pitch);
                copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copies[0].imageExtent = {static_cast<std::uint32_t>(visible_width),
                                         static_cast<std::uint32_t>(visible_height), 1};
                copies[1].bufferOffset = static_cast<VkDeviceSize>(video.pitch) *
                                             video.source_height +
                                         static_cast<VkDeviceSize>(crop_top / 2) * video.pitch +
                                         static_cast<VkDeviceSize>(crop_left & ~1);
                copies[1].bufferRowLength = static_cast<std::uint32_t>(video.pitch / 2);
                copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copies[1].imageExtent = {static_cast<std::uint32_t>(chroma_width),
                                         static_cast<std::uint32_t>(chroma_height), 1};
                g_cmd_copy_buffer_to_image(g_command, g_video_staging, g_video_y,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[0]);
                g_cmd_copy_buffer_to_image(g_command, g_video_staging, g_video_uv,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[1]);

                VkImageMemoryBarrier video_to_shader[2]{};
                video_to_shader[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                video_to_shader[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_to_shader[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                video_to_shader[0].image = g_video_y;
                video_to_shader[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                video_to_shader[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                video_to_shader[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                video_to_shader[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                video_to_shader[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                video_to_shader[1] = video_to_shader[0];
                video_to_shader[1].image = g_video_uv;
                g_cmd_pipeline_barrier(g_command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                       nullptr, 2, video_to_shader);
                g_video_y_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                g_video_uv_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                g_video_serial = video.serial;
                static bool logged = false;
                if (!logged) {
                    system::log("vulkan: video upload %dx%d crop=%d,%d pitch=%d serial=%llu",
                                visible_width, visible_height, crop_left, crop_top, video.pitch,
                                static_cast<unsigned long long>(video.serial));
                    logged = true;
                }
            }
        }
    }
    if (!video_active) g_video_serial = 0;

    VideoPush push{};
    if (video_active) {
        const int rect_x = std::clamp(video.rect_x, 0, kWidth);
        const int rect_y = std::clamp(video.rect_y, 0, kHeight);
        const int rect_width = std::clamp(video.rect_width, 0, kWidth - rect_x);
        const int rect_height = std::clamp(video.rect_height, 0, kHeight - rect_y);
        if (rect_width > 0 && rect_height > 0 && video.visible_height > 0) {
            const double aspect =
                static_cast<double>(video.visible_width) / video.visible_height;
            int draw_width = rect_width;
            int draw_height = static_cast<int>(draw_width / aspect + 0.5);
            if (draw_height > rect_height) {
                draw_height = rect_height;
                draw_width = static_cast<int>(draw_height * aspect + 0.5);
            }
            draw_width = std::clamp(draw_width, 1, rect_width);
            draw_height = std::clamp(draw_height, 1, rect_height);
            const int draw_x = rect_x + (rect_width - draw_width) / 2;
            const int draw_y = rect_y + (rect_height - draw_height) / 2;
            push.rect[0] = static_cast<float>(rect_x);
            push.rect[1] = static_cast<float>(rect_y);
            push.rect[2] = static_cast<float>(rect_width);
            push.rect[3] = static_cast<float>(rect_height);
            push.draw[0] = static_cast<float>(draw_x);
            push.draw[1] = static_cast<float>(draw_y);
            push.draw[2] = static_cast<float>(draw_width);
            push.draw[3] = static_cast<float>(draw_height);
            push.flags[0] = 1.0f;
            push.flags[1] = video.full_range ? 1.0f : 0.0f;
        } else {
            video_active = false;
        }
    }

    VkClearValue clear{};
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = g_render_pass;
    render_begin.framebuffer = g_framebuffer[g_buffer_index];
    render_begin.renderArea.extent = {kWidth, kHeight};
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear;
    g_cmd_begin_render_pass(g_command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    g_cmd_bind_pipeline(g_command, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
    g_cmd_bind_descriptor_sets(g_command, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline_layout, 0,
                               1, &g_descriptor_set, 0, nullptr);
    g_cmd_push_constants(g_command, g_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), &push);
    g_cmd_draw(g_command, 3, 1, 0, 0);
    g_cmd_end_render_pass(g_command);

    const VkResult end_result = g_end_command_buffer(g_command);
    if (end_result != VK_SUCCESS) return fail("end", end_result);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_command;
    const VkResult submit_result = g_queue_submit(g_queue, 1, &submit, g_fence);
    if (submit_result != VK_SUCCESS) return fail("submit", submit_result);
    const VkResult wait_result = g_wait_for_fences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
    if (wait_result != VK_SUCCESS) return fail("fence", wait_result);

    const int flip = sceVideoOutSubmitFlip(g_video_out, g_buffer_index,
                                           ORBIS_VIDEO_OUT_FLIP_VSYNC,
                                           static_cast<std::int64_t>(g_frame));
    if (flip < 0) return fail("flip", flip);
    OrbisKernelEvent event{};
    int count = 0;
    OrbisKernelUseconds timeout = 500000;
    const int waited = sceKernelWaitEqueue(g_flip_queue, &event, 1, &count, &timeout);
    if (waited != 0 && failures++ < 8)
        system::log("vulkan: flip wait=0x%x", static_cast<unsigned>(waited));

    ++g_frame;
    g_buffer_index ^= 1;
    if (g_frame == 1) system::log("vulkan: first frame presented");
    return true;
}

void vulkan_display_close() {
    if (g_device && g_device_wait_idle) g_device_wait_idle(g_device);
    // Stop scanout before the images and their memory go away.
    if (g_video_out >= 0) {
        sceVideoOutUnregisterBuffers(g_video_out, 0);
        sceVideoOutClose(g_video_out);
        g_video_out = -1;
    }
    if (g_flip_queue) {
        sceKernelDeleteEqueue(g_flip_queue);
        g_flip_queue = 0;
    }
    if (g_device && g_destroy_pipeline && g_pipeline) g_destroy_pipeline(g_device, g_pipeline, nullptr);
    if (g_device && g_destroy_pipeline_layout && g_pipeline_layout)
        g_destroy_pipeline_layout(g_device, g_pipeline_layout, nullptr);
    if (g_device && g_destroy_shader_module && g_fragment_shader)
        g_destroy_shader_module(g_device, g_fragment_shader, nullptr);
    if (g_device && g_destroy_shader_module && g_vertex_shader)
        g_destroy_shader_module(g_device, g_vertex_shader, nullptr);
    for (int i = 0; i < kBufferCount; ++i)
        if (g_device && g_destroy_framebuffer && g_framebuffer[i])
            g_destroy_framebuffer(g_device, g_framebuffer[i], nullptr);
    if (g_device && g_destroy_render_pass && g_render_pass)
        g_destroy_render_pass(g_device, g_render_pass, nullptr);
    if (g_device && g_destroy_descriptor_pool && g_descriptor_pool)
        g_destroy_descriptor_pool(g_device, g_descriptor_pool, nullptr);
    if (g_device && g_destroy_set_layout && g_set_layout)
        g_destroy_set_layout(g_device, g_set_layout, nullptr);
    destroy_video_staging();
    destroy_video_textures();
    if (g_device && g_destroy_sampler && g_sampler) g_destroy_sampler(g_device, g_sampler, nullptr);
    if (g_device && g_destroy_image_view && g_texture_view)
        g_destroy_image_view(g_device, g_texture_view, nullptr);
    if (g_device && g_destroy_image && g_texture) g_destroy_image(g_device, g_texture, nullptr);
    if (g_device && g_unmap_memory && g_staging_memory && g_staging_mapped)
        g_unmap_memory(g_device, g_staging_memory);
    if (g_device && g_destroy_buffer && g_staging) g_destroy_buffer(g_device, g_staging, nullptr);
    if (g_device && g_free_memory && g_texture_memory)
        g_free_memory(g_device, g_texture_memory, nullptr);
    if (g_device && g_free_memory && g_staging_memory)
        g_free_memory(g_device, g_staging_memory, nullptr);
    for (int i = 0; i < kBufferCount; ++i) {
        if (g_device && g_destroy_image_view && g_scanout_view[i])
            g_destroy_image_view(g_device, g_scanout_view[i], nullptr);
        if (g_device && g_destroy_image && g_scanout[i])
            g_destroy_image(g_device, g_scanout[i], nullptr);
        if (g_device && g_unmap_memory && g_scanout_memory[i] && g_scanout_mapped[i])
            g_unmap_memory(g_device, g_scanout_memory[i]);
        if (g_device && g_free_memory && g_scanout_memory[i])
            g_free_memory(g_device, g_scanout_memory[i], nullptr);
    }
    if (g_device && g_destroy_fence && g_fence) g_destroy_fence(g_device, g_fence, nullptr);
    if (g_device && g_destroy_command_pool && g_pool)
        g_destroy_command_pool(g_device, g_pool, nullptr);
    if (g_device && g_destroy_device) g_destroy_device(g_device, nullptr);
    if (g_destroy_instance && g_instance) g_destroy_instance(g_instance, nullptr);

    g_instance = VK_NULL_HANDLE;
    g_physical = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_queue = VK_NULL_HANDLE;
    g_queue_family = -1;
    g_video_out = -1;
    g_flip_queue = 0;
    g_frame = 0;
    g_buffer_index = 0;
    for (int i = 0; i < kBufferCount; ++i) {
        g_scanout[i] = VK_NULL_HANDLE;
        g_scanout_memory[i] = VK_NULL_HANDLE;
        g_scanout_view[i] = VK_NULL_HANDLE;
        g_framebuffer[i] = VK_NULL_HANDLE;
        g_scanout_mapped[i] = nullptr;
        g_scanout_address[i] = nullptr;
    }
    g_scanout_pitch = 0;
    g_staging = VK_NULL_HANDLE;
    g_staging_memory = VK_NULL_HANDLE;
    g_staging_mapped = nullptr;
    g_texture = VK_NULL_HANDLE;
    g_texture_memory = VK_NULL_HANDLE;
    g_texture_view = VK_NULL_HANDLE;
    g_sampler = VK_NULL_HANDLE;
    g_texture_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_set_layout = VK_NULL_HANDLE;
    g_descriptor_pool = VK_NULL_HANDLE;
    g_descriptor_set = VK_NULL_HANDLE;
    g_vertex_shader = VK_NULL_HANDLE;
    g_fragment_shader = VK_NULL_HANDLE;
    g_pipeline_layout = VK_NULL_HANDLE;
    g_render_pass = VK_NULL_HANDLE;
    g_pipeline = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_command = VK_NULL_HANDLE;
    g_fence = VK_NULL_HANDLE;
}

} // namespace ps4
