// Vulkan/RADV presentation for the PS4 runtime.
//
// The first stage owns a linear 1920x1080 image registered directly with
// sceVideoOut and clears it through Vulkan. The next stage uploads the engine
// framebuffer (and later the decoded NV12 planes) as textures.
#pragma once

#include <cstdint>

namespace ps4 {

bool vulkan_display_open(int width, int height);
bool vulkan_display_present(const std::uint32_t* pixels);
void vulkan_display_close();

} // namespace ps4
