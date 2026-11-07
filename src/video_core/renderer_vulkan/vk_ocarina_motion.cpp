// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef EGLANTINE_STREAMLINE
#include <cstring>
#include "common/logging.h"
#include "video_core/ocarina_dlss_debug.h"
#include "video_core/renderer_vulkan/vk_ocarina_motion.h"
#include "video_core/host_shaders/ocarina_motion_comp_spv.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
constexpr auto bindings = [] {
    std::array<VkDescriptorSetLayoutBinding, 9> result{};
    for (u32 i = 0; i < result.size(); ++i)
        result[i] = {
            i, i == 8 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
               (i < 2 || i == 5 || i == 7) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    return result;
}();
constexpr auto entries = [] {
    std::array<VkDescriptorUpdateTemplateEntry, bindings.size()> result{};
    for (u32 i = 0; i < result.size(); ++i)
        result[i] = {i,
                     0,
                     1,
                     bindings[i].descriptorType,
                     i * sizeof(DescriptorUpdateEntry),
                     sizeof(DescriptorUpdateEntry)};
    return result;
}();
using GeometrySampleCounts = std::array<u32, 8>;
struct MotionParams {
    u32 reset, packed_color, use_camera, save_depth;
    std::array<float, 16> clip_to_previous;
    std::array<float, 2> viewport_sign;
    u32 has_geometry, sample_geometry, geometry_for_sr;
};
static_assert(sizeof(MotionParams) == 100);
constexpr DescriptorBankInfo bank{.storage_buffers=1,.textures = 4, .images = 4, .score = 9};
constexpr std::array<VkPushConstantRange, 1> push{{{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MotionParams)}}};
constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
} // namespace
OcarinaMotion::OcarinaMotion(const Device& dev, Scheduler& sched, DescriptorPool& pool,
                             ComputePassDescriptorQueue& descriptors, MemoryAllocator& allocator,
                             VkExtent2D size)
    : ComputePass(dev, sched, pool, bindings, entries, bank, push, OCARINA_MOTION_COMP_SPV),
      extent(size), scheduler(sched), allocator(allocator), queue(descriptors),
      sampler(CreateWrappedSampler(dev, VK_FILTER_LINEAR)),
      flow(CreateWrappedImage(allocator, size, VK_FORMAT_R16G16_SFLOAT,
                              Streamline::IsDlssGAvailable() ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)),
      linear_color(CreateWrappedImage(allocator, size, VK_FORMAT_R16G16B16A16_SFLOAT)),
      linear_color_view(CreateWrappedImageView(dev, linear_color, VK_FORMAT_R16G16B16A16_SFLOAT)),
      flow_view(CreateWrappedImageView(dev, flow, VK_FORMAT_R16G16_SFLOAT)) {
    geometry_stats = allocator.CreateBuffer(VkBufferCreateInfo{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=sizeof(GeometrySampleCounts),.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT},MemoryUsage::Download);
    for (u32 i = 0; i < 2; ++i) {
        history[i] = CreateWrappedImage(allocator, size, VK_FORMAT_R16G16_SFLOAT);
        history_views[i] = CreateWrappedImageView(dev, history[i], VK_FORMAT_R16G16_SFLOAT);
    }
    if (Streamline::IsDlssGAvailable()) {
        fg_flow = CreateWrappedImage(allocator, size, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        fg_view = CreateWrappedImageView(dev, fg_flow, VK_FORMAT_R16G16_SFLOAT);
        depth_sampler = dev.GetLogical().CreateSampler(VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST});
    }
    sched.RequestOutsideRenderPassOperationContext();
    sched.Record([images = std::array<VkImage, 5>{*flow, *linear_color, *history[0], *history[1], *fg_flow}](
                     vk::CommandBuffer cmd) {
        for (auto image : images) {
            if (!image) continue;
            const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                                VK_ACCESS_SHADER_WRITE_BIT,
                                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .image = image,
                                               .subresourceRange = range};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
        }
    });
}
StreamlineDlssUpscaler::Image OcarinaMotion::LinearColor() const {
    return {*linear_color_view, *linear_color, VK_FORMAT_R16G16B16A16_SFLOAT, range,
            VK_IMAGE_ASPECT_COLOR_BIT};
}
std::optional<StreamlineDlssUpscaler::Image> OcarinaMotion::FrameGenerationMotion() const {
    if (!fg_motion_valid) return std::nullopt;
    return StreamlineDlssUpscaler::Image{*fg_view, *fg_flow, VK_FORMAT_R16G16_SFLOAT, range,
                                         VK_IMAGE_ASPECT_COLOR_BIT, fg_flow.UsageFlags()};
}

StreamlineDlssUpscaler::Image OcarinaMotion::Estimate(VkImageView color, VkImageView depth, bool reset,
        bool packed_color, const std::optional<OcarinaCamera>& camera, u64 scene_frame,
        std::span<const OcarinaGeometryDraw> geometry) {
    scheduler.RequestOutsideRenderPassOperationContext();
    if (!fg_flow || last_scene_frame != scene_frame) {
        current ^= 1;
        previous_camera = last_scene_frame && scene_frame == *last_scene_frame + 1
                            ? current_camera : std::nullopt;
        last_scene_frame = scene_frame;
    }
    if (reset) previous_camera.reset();
    current_camera = camera;
    fg_motion_valid = fg_flow && previous_camera && camera && !first &&
                      previous_camera->viewport_sign == camera->viewport_sign;
    VkImageView geometry_view{};
    if (fg_motion_valid && !geometry.empty()) {
        if (!geometry_pass) geometry_pass.emplace(device, allocator, extent);
        geometry_view = geometry_pass->Render(scheduler, geometry);
    }
    if (stats_tick && scheduler.IsFree(*stats_tick)) {
        geometry_stats.Invalidate();
        GeometrySampleCounts counts{};
        std::memcpy(counts.data(),geometry_stats.Mapped().data(),sizeof(counts));
        LOG_INFO(Render_Vulkan,
                 "Ocarina projected geometry samples (1/64 pixels): {} covered, {} depth-matched, "
                 "{} moving, {} halo, {} equipment depth-matched, {} equipment moving, {} other depth-matched, {} other moving",
                 counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7]);
        stats_tick.reset();
    }
    queue.Acquire(scheduler, bindings.size());
    queue.AddSampledImage(color, *sampler);
    queue.AddSampledImage(*history_views[current ^ 1], *sampler);
    queue.AddImage(*flow_view);
    queue.AddImage(*history_views[current]);
    queue.AddImage(*linear_color_view);
    queue.AddSampledImage(fg_flow ? depth : color, fg_flow ? *depth_sampler : *sampler);
    queue.AddImage(fg_flow ? *fg_view : *flow_view);
    queue.AddSampledImage(geometry_view ? geometry_view : color, fg_flow ? *depth_sampler : *sampler);
    queue.AddBuffer(*geometry_stats, 0, sizeof(GeometrySampleCounts));
    const auto data = queue.UpdateData();
    MotionParams params{reset || first, packed_color, fg_motion_valid, fg_flow ? 1u : 0u};
    params.has_geometry = geometry_view != VK_NULL_HANDLE;
    params.geometry_for_sr = VideoCore::Nx2Debug::ocarina_sr_geometry.load(std::memory_order_relaxed);
    if (geometry_view && !stats_tick && scene_frame >= next_stats_frame) {
        params.sample_geometry = 1;
        stats_tick = scheduler.CurrentTick();
        next_stats_frame = scene_frame + 120;
    }
    if (fg_motion_valid) {
        params.clip_to_previous = camera->ClipToPrevious(*previous_camera);
        params.viewport_sign = camera->viewport_sign;
    }
    scheduler.Record([this, data, params](vk::CommandBuffer cmd) {
        if (params.sample_geometry) {
            cmd.FillBuffer(*geometry_stats, 0, sizeof(GeometrySampleCounts), 0);
        }
        const VkMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
        const auto set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, data);
        cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        cmd.Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        if (params.sample_geometry) {
            const VkMemoryBarrier host_read{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,host_read);
        }
    });
    first = false;
    scheduler.InvalidateState();
    return {*flow_view, *flow, VK_FORMAT_R16G16_SFLOAT, range, VK_IMAGE_ASPECT_COLOR_BIT,
            flow.UsageFlags()};
}
} // namespace Vulkan
#endif
