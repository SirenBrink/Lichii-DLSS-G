// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS (through NVIDIA Streamline) in place of a game's DLSS evaluation.

#include <array>
#include <cstring>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "video_core/dlss_evaluation.h"
#include "video_core/ocarina_dlss_debug.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include <sl_matrix_helpers.h>

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
// Streamline clears the output before DLSS writes it.
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

int EnvInt(const char* name, int fallback) {
    const char* const value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

// DLSS validates the render size against the mode's range: pick the mode for this ratio.
sl::DLSSMode ModeFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(render.width) / static_cast<f32>(upscale.width);
    if (ratio >= 0.99f) {
        return sl::DLSSMode::eDLAA;
    }
    if (ratio >= 0.65f) {
        return sl::DLSSMode::eMaxQuality;
    }
    if (ratio >= 0.57f) {
        return sl::DLSSMode::eBalanced;
    }
    if (ratio >= 0.49f) {
        return sl::DLSSMode::eMaxPerformance;
    }
    return sl::DLSSMode::eUltraPerformance;
}

// Preset K for every mode: NVIDIA's defaults for Performance/Ultra Performance (M, L) draw thin
// horizontal lines in Fire Emblem at 2x resolution scale. EDEN_NX2_DLSS_PRESET=J|K|L|M picks
// another model, EDEN_NX2_DLSS_PRESET=default leaves the choice to NVIDIA.
sl::DLSSPreset Preset() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M') {
        return static_cast<sl::DLSSPreset>(static_cast<u32>(sl::DLSSPreset::ePresetJ) +
                                           static_cast<u32>(value[0] - 'J'));
    }
    if (value && std::string_view{value} == "default") {
        return sl::DLSSPreset::eDefault;
    }
    return sl::DLSSPreset::ePresetK;
}

sl::float4x4 Identity() {
    sl::float4x4 matrix;
    matrix.setRow(0, {1.0f, 0.0f, 0.0f, 0.0f});
    matrix.setRow(1, {0.0f, 1.0f, 0.0f, 0.0f});
    matrix.setRow(2, {0.0f, 0.0f, 1.0f, 0.0f});
    matrix.setRow(3, {0.0f, 0.0f, 0.0f, 1.0f});
    return matrix;
}

sl::float4x4 ViewToWorld(const OcarinaCamera& camera) {
    const auto& v = camera.view;
    sl::float4x4 world_to_view{};
    for (u32 row = 0; row < 4; ++row) {
        world_to_view.setRow(row, {v[row], v[4 + row], v[8 + row], row == 3 ? 1.f : 0.f});
    }
    sl::float4x4 result{};
    sl::matrixOrthoNormalInvert(result, world_to_view);
    return result;
}

enum class ClipSpace { Guest, Streamline };

sl::float4x4 ViewToClip(const OcarinaCamera& camera, ClipSpace clip_space) {
    // The captured projection is right-handed OpenGL. Streamline's row-vector
    // projection must map to the normalized depth stored in the Z32 image.
    const auto& p = camera.projection;
    sl::float4x4 result{};
    // NVIDIA converts a Vulkan texel's UV to (2u-1, 1-2v). The guest's
    // effective viewport, including origin and swizzle, can use opposite axes.
    const bool sdk = clip_space == ClipSpace::Streamline;
    result.setRow(0, {p[0] * (sdk ? camera.viewport_sign[0] : 1.f), 0, 0, 0});
    result.setRow(1, {0, p[5] * (sdk ? -camera.viewport_sign[1] : 1.f), 0, 0});
    result.setRow(2, {0, 0, (p[10] - 1.f) * 0.5f, -1.f});
    result.setRow(3, {0, 0, p[11] * 0.5f, 0});
    return result;
}

void SetOcarinaCamera(sl::Constants& constants, const OcarinaCamera& current,
                      const OcarinaCamera& previous, ClipSpace clip_space) {
    const auto world = ViewToWorld(current);
    sl::float4x4 camera_to_prev{}, clip_to_prev_camera{};
    sl::calcCameraToPrevCamera(camera_to_prev, world, ViewToWorld(previous));
    constants.cameraViewToClip = ViewToClip(current, clip_space);
    sl::matrixFullInvert(constants.clipToCameraView, constants.cameraViewToClip);
    sl::matrixMul(clip_to_prev_camera, constants.clipToCameraView, camera_to_prev);
    sl::matrixMul(constants.clipToPrevClip, clip_to_prev_camera, ViewToClip(previous, clip_space));
    sl::matrixFullInvert(constants.prevClipToClip, constants.clipToPrevClip);
    constants.cameraPos = {world[3].x, world[3].y, world[3].z};
    constants.cameraRight = {world[0].x, world[0].y, world[0].z};
    constants.cameraUp = {world[1].x, world[1].y, world[1].z};
    constants.cameraFwd = {-world[2].x, -world[2].y, -world[2].z};
    const auto& p = current.projection;
    constants.cameraNear = p[11] / (p[10] - 1.f);
    constants.cameraFar = p[11] / (p[10] + 1.f);
    constants.cameraFOV = 2.f * std::atan(1.f / p[5]);
    constants.cameraAspectRatio = p[5] / p[0];
}

VkImageMemoryBarrier Barrier(const StreamlineDlssUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

sl::Resource Resource(VkImage image, VkImageView view, VkFormat format, VkImageLayout layout,
                      VkExtent2D size, VkImageUsageFlags usage) {
    sl::Resource resource{sl::ResourceType::eTex2d, image, nullptr, view,
                          static_cast<uint32_t>(layout)};
    resource.width = size.width;
    resource.height = size.height;
    resource.nativeFormat = static_cast<uint32_t>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = usage;
    return resource;
}

} // Anonymous namespace

std::array<float, 16> OcarinaCamera::ClipToPrevious(const OcarinaCamera& previous) const {
    sl::Constants constants{};
    SetOcarinaCamera(constants, *this, previous, ClipSpace::Guest);
    std::array<float, 16> matrix{};
    // Streamline row vectors and GLSL column vectors use the same bytes here.
    static_assert(sizeof(matrix) == sizeof(constants.clipToPrevClip));
    std::memcpy(matrix.data(), &constants.clipToPrevClip, sizeof(matrix));
    return matrix;
}

StreamlineDlssUpscaler::StreamlineDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                               MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!Streamline::IsDlssAvailable()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    const sl::Result result = Streamline::GetApi().get_feature_function(
        sl::kFeatureDLSS, "slDLSSSetOptions", set_options);
    if (result != sl::Result::eOk || !set_options) {
        LOG_ERROR(Render_Vulkan, "Streamline's DLSS has no slDLSSSetOptions (result {})",
                  static_cast<int>(result));
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

StreamlineDlssUpscaler::~StreamlineDlssUpscaler() {
    // Not scheduler.Finish(): this runs when the renderer goes away, after the game's channels are
    // released, and a submit would touch their state. Nothing more gets recorded by then.
    void(device.GetLogical().WaitIdle());
    if (Streamline::IsDlssAvailable()) {
        Streamline::GetApi().free_resources(sl::kFeatureDLSS, sl::ViewportHandle{0u});
    }
}

void StreamlineDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool StreamlineDlssUpscaler::Upscale(const Inputs& in) {
    if (failed) {
        return false;
    }
    bool reset = in.reset;
    bool options_changed = false;
    if (!output_image || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height ||
        in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
        options_changed = true;
        reset = true;
    }

    // The game's own setup (Fortune's Weave: render-size motion vectors, reversed depth). Its
    // colour is linear and goes above 1 with no exposure passed, so by default DLSS gets the HDR
    // path with its own auto exposure. EDEN_NX2_DLSS_HDR=0 / EDEN_NX2_DLSS_AUTO_EXPOSURE=0 turn
    // those off.
    sl::DLSSOptions options{};
    options.mode = ModeFor(in.render_size, in.upscale_size);
    options.outputWidth = in.upscale_size.width;
    options.outputHeight = in.upscale_size.height;
    options.colorBuffersHDR = EnvInt("EDEN_NX2_DLSS_HDR", 1) != 0 ? sl::eTrue : sl::eFalse;
    options.useAutoExposure =
        EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1) != 0 ? sl::eTrue : sl::eFalse;
    const sl::DLSSPreset preset = Preset();
    options.dlaaPreset = preset;
    options.qualityPreset = preset;
    options.balancedPreset = preset;
    options.performancePreset = preset;
    options.ultraPerformancePreset = preset;
    options.ultraQualityPreset = preset;
    if (options_changed) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS (Streamline): {}x{} -> {}x{}, mode {}, preset {}",
                 in.render_size.width, in.render_size.height, in.upscale_size.width,
                 in.upscale_size.height, static_cast<int>(options.mode),
                 static_cast<int>(preset));
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, options, options_changed, index = frame_index++,
                      output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        const Streamline::Api& api = Streamline::GetApi();
        const sl::ViewportHandle viewport{0u};
        // The caller draws the output this frame even when DLSS writes nothing to it: clear it
        // then, rather than show undefined contents (FSR 3.1 takes over from the next frame).
        const auto clear_output = [&] {
            const VkImageSubresourceRange range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
                return VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = src_access,
                    .dstAccessMask = dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = output_image_handle,
                    .subresourceRange = range,
                };
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT));
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        };
        sl::FrameToken* frame{};
        const bool request_fg = in.ocarina_scene && Streamline::IsDlssGAvailable();
        if (request_fg) {
            Streamline::BeginFrameGenerationRender();
            frame = Streamline::GetFrameGenerationToken();
            if (frame) {
                // Only the last scene resolve before present can describe that real frame.
                Streamline::MarkFrameGenerationTagged(*frame, 0, 0);
                const u32 token = u32(*frame);
                if (fg_camera_token != token) {
                    previous_fg_camera = fg_camera_token && *fg_camera_token + 1u == token
                                             ? current_fg_camera : std::nullopt;
                    fg_camera_token = token;
                }
            }
        }
        // Repeat resolves replace this frame's candidate, but always reproject from
        // the preceding real frame. SR commits the camera even on the initial real
        // frame, when the motion estimator cannot yet provide guided vectors.
        current_fg_camera.reset();
        if (reset || !frame) {
            previous_fg_camera.reset();
        }
        if (!frame) {
            fg_camera_token.reset();
        }
        const bool has_fg_token = frame != nullptr;
        const bool fg_camera_history = has_fg_token && !reset && in.camera && previous_fg_camera;

        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs come from the game's draws and dispatches and stay in GENERAL otherwise;
        // DLSS reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };

        // With camera motion included in SR's flow, it doesn't need the FG camera to keep
        // accumulating. Only an actual SR input/history discontinuity should reset SR.
        const f32 rw = static_cast<f32>(in.render_size.width);
        const f32 rh = static_cast<f32>(in.render_size.height);
        sl::Constants constants{};
        constants.cameraViewToClip = Identity();
        constants.clipToCameraView = Identity();
        constants.clipToLensClip = Identity();
        constants.clipToPrevClip = Identity();
        constants.prevClipToClip = Identity();
        constants.jitterOffset = {in.jitter[0], in.jitter[1]};
        // Streamline wants motion in [-1,1] screen units; ours turns into render pixels.
        constants.mvecScale = {in.motion_vector_scale[0] / rw, in.motion_vector_scale[1] / rh};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {0.0f, 0.0f, 0.0f};
        constants.cameraUp = {0.0f, 1.0f, 0.0f};
        constants.cameraRight = {1.0f, 0.0f, 0.0f};
        constants.cameraFwd = {0.0f, 0.0f, 1.0f};
        constants.cameraNear = 0.1f;
        constants.cameraFar = 1000.0f;
        constants.cameraFOV = 1.0f;
        constants.cameraAspectRatio = rw / rh;
        constants.depthInverted =
            in.depth_direction == DlssDepth::Reversed ? sl::eTrue : sl::eFalse;
        constants.cameraMotionIncluded = sl::eTrue;
        constants.motionVectors3D = sl::eFalse;
        constants.reset = reset ? sl::eTrue : sl::eFalse;
        constants.motionVectorsJittered = sl::eFalse;
        if (request_fg && in.camera) {
            frames_waiting_for_camera = 0;
            SetOcarinaCamera(constants, *in.camera,
                             fg_camera_history ? *previous_fg_camera : *in.camera,
                             ClipSpace::Streamline);
            // In Streamline's 1/(1-z) units the SDK's default separation (40)
            // misses close foreground/background edges in Kokiri. A gap of 10
            // separates Link from nearby ground without splitting his mesh.
            constants.minRelativeLinearDepthObjectSeparation = 10.f;
            if (!reset && !fg_camera_history && camera_gap_reports < 3) {
                ++camera_gap_reports;
                LOG_INFO(Render_Vulkan, "Ocarina DLSS kept SR history across a frame-generation "
                                        "camera gap; presenting a real frame");
            }
            if (!reported_fg_camera) {
                reported_fg_camera = true;
                LOG_INFO(Render_Vulkan, "Ocarina DLSS-G matched the scene camera: vertical FOV "
                                        "{} radians, near {}, far {}",
                         constants.cameraFOV, constants.cameraNear, constants.cameraFar);
            }
        } else if (request_fg && ++frames_waiting_for_camera == 30) {
            LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G: 30 scene resolves without a matching "
                                       "main-view camera; presenting real frames only");
        }

        sl::Resource color = Resource(in.color.image, in.color.view, in.color.format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                      VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource depth = Resource(in.depth.image, in.depth.view, in.depth.format,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                      VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource motion = Resource(in.motion_vectors.image, in.motion_vectors.view,
                                       in.motion_vectors.format,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, in.render_size,
                                       VK_IMAGE_USAGE_SAMPLED_BIT);
        sl::Resource output = Resource(output_image_handle, output_view_handle, OUTPUT_FORMAT,
                                       VK_IMAGE_LAYOUT_GENERAL, in.upscale_size, OUTPUT_USAGE);
        const sl::Extent render_area{0, 0, in.render_size.width, in.render_size.height};
        const sl::Extent output_area{0, 0, in.upscale_size.width, in.upscale_size.height};
        // Local tags: everything stays put until the evaluation is recorded.
        sl::ResourceTag color_tag{&color, sl::kBufferTypeScalingInputColor,
                                  sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag depth_tag{&depth, sl::kBufferTypeDepth,
                                  sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag motion_tag{&motion, sl::kBufferTypeMotionVectors,
                                   sl::ResourceLifecycle::eValidUntilEvaluate, &render_area};
        sl::ResourceTag output_tag{&output, sl::kBufferTypeScalingOutputColor,
                                   sl::ResourceLifecycle::eValidUntilEvaluate, &output_area};
        const sl::BaseStructure* inputs[] = {&viewport, &color_tag, &depth_tag, &motion_tag,
                                             &output_tag};
        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        sl::Result api_result = sl::Result::eOk;
        const auto result = VideoCommon::RecordDlssEvaluation(
            [&] {
                if (!options_changed) {
                    return true;
                }
                const auto set = reinterpret_cast<PFun_slDLSSSetOptions*>(set_options);
                api_result = set(viewport, options);
                return api_result == sl::Result::eOk;
            },
            [&] {
                if (has_fg_token) {
                    return true;
                }
                api_result = api.get_new_frame_token(frame, &index);
                return api_result == sl::Result::eOk && frame;
            },
            [&] {
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
            },
            [&] {
                api_result = api.set_constants(constants, *frame, viewport);
                return api_result == sl::Result::eOk;
            },
            [&] {
                api_result = api.evaluate_feature(sl::kFeatureDLSS, *frame, inputs, std::size(inputs),
                                                  reinterpret_cast<sl::CommandBuffer*>(*cmdbuf));
                if (api_result == sl::Result::eOk && has_fg_token) {
                    current_fg_camera = in.camera;
                }
                if (api_result == sl::Result::eOk && fg_camera_history && in.frame_generation_motion &&
                    in.depth.range.baseMipLevel == 0 && in.depth.range.baseArrayLayer == 0 &&
                    (in.depth.usage & in.frame_generation_motion->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
                    // Both inputs can be overwritten by the next dispatch. Streamline records
                    // its own immutable copies here, while their read-only layouts still apply.
                    sl::Resource fg_depth = depth;
                    fg_depth.usage = in.depth.usage;
                    const auto& fg = *in.frame_generation_motion;
                    sl::Resource fg_motion = Resource(fg.image, fg.view, fg.format, VK_IMAGE_LAYOUT_GENERAL,
                                                      in.render_size, fg.usage);
                    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                        Barrier(fg, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
                    const sl::ResourceTag fg_tags[]{
                        {&fg_depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow,
                         &render_area},
                        {&fg_motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow,
                         &render_area},
                    };
                    const auto tag = api.set_tag_for_frame(
                        *frame, viewport, fg_tags, std::size(fg_tags),
                        reinterpret_cast<sl::CommandBuffer*>(*cmdbuf));
                    if (tag == sl::Result::eOk) {
                        Streamline::MarkFrameGenerationTagged(*frame, in.upscale_size.width,
                                                              in.upscale_size.height);
                        if (!reported_fg_tags) {
                            reported_fg_tags = true;
                            LOG_INFO(Render_Vulkan, "Ocarina DLSS-G tagged scene depth and "
                                     "camera/depth-guided motion with moving-surface search {}x{} for real frame {}",
                                     in.render_size.width, in.render_size.height, u32(*frame));
                        }
                    } else {
                        if (!reported_fg_failure) {
                            reported_fg_failure = true;
                            LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G resource tags failed "
                                                       "(Streamline result {})", int(tag));
                        }
                    }
                }
                return api_result == sl::Result::eOk;
            },
            [&] {
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                                       to_general);
            });
        if (result == VideoCommon::DlssEvaluationResult::Succeeded && in.ocarina_scene) {
            VideoCore::Nx2Debug::ocarina_evaluations.fetch_add(1, std::memory_order_relaxed);
        }
        if (result == VideoCommon::DlssEvaluationResult::Succeeded &&
            (!reported_success || options_changed)) {
            reported_success = true;
            LOG_INFO(Render_Vulkan,
                     "NVIDIA DLSS Super Resolution evaluated successfully: {}x{} -> {}x{} (mode {})",
                     in.render_size.width, in.render_size.height, in.upscale_size.width,
                     in.upscale_size.height, static_cast<int>(options.mode));
        }
        if (result != VideoCommon::DlssEvaluationResult::Succeeded && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan,
                      "NVIDIA DLSS recording failed at stage {} (Streamline result {}); using FSR 3.1",
                      static_cast<int>(result), static_cast<int>(api_result));
        }
        if (result != VideoCommon::DlssEvaluationResult::Succeeded) {
            clear_output();
        }
    });
    // DLSS binds its own pipelines and descriptors on the command buffer.
    scheduler.InvalidateState();
    return true;
}

} // namespace Vulkan
