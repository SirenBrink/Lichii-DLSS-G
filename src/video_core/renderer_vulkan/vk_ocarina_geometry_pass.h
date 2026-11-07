// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_ocarina_geometry.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
class Device;
class Scheduler;
class OcarinaGeometryPass {
public:
    OcarinaGeometryPass(const Device& device, MemoryAllocator& allocator, VkExtent2D size);
    VkImageView Render(Scheduler& scheduler, std::span<const OcarinaGeometryDraw> draws);
private:
    VkExtent2D extent;
    vk::Image color, depth;
    vk::ImageView color_view, depth_view;
    vk::RenderPass pass;
    vk::Framebuffer framebuffer;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
};
} // namespace Vulkan
