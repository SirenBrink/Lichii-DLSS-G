// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_ocarina_geometry_pass.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"

namespace Vulkan {
class OcarinaMotion final : public ComputePass {
  public:
    OcarinaMotion(const Device& device, Scheduler& scheduler, DescriptorPool& pool,
                  ComputePassDescriptorQueue& queue, MemoryAllocator& allocator, VkExtent2D size);
    StreamlineDlssUpscaler::Image Estimate(VkImageView color, VkImageView depth, bool reset,
                                          bool packed_color, const std::optional<OcarinaCamera>& camera,
                                          u64 scene_frame, std::span<const OcarinaGeometryDraw> geometry);
    std::optional<StreamlineDlssUpscaler::Image> FrameGenerationMotion() const;
    StreamlineDlssUpscaler::Image LinearColor() const;
    const VkExtent2D extent;

  private:
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    ComputePassDescriptorQueue& queue;
    vk::Sampler sampler;
    vk::Image flow;
    vk::Image linear_color;
    vk::ImageView linear_color_view;
    std::array<vk::Image, 2> history;
    vk::ImageView flow_view;
    vk::Image fg_flow;
    vk::ImageView fg_view;
    vk::Sampler depth_sampler;
    std::optional<OcarinaGeometryPass> geometry_pass;
    vk::Buffer geometry_stats;
    std::optional<u64> stats_tick;
    u64 next_stats_frame{};
    std::optional<OcarinaCamera> previous_camera;
    std::optional<OcarinaCamera> current_camera;
    std::optional<u64> last_scene_frame;
    bool fg_motion_valid{};
    std::array<vk::ImageView, 2> history_views;
    u32 current{};
    bool first{true};
};
} // namespace Vulkan
