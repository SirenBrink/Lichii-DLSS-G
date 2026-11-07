// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_vulkan/vk_ocarina_geometry_pass.h"
#include "video_core/host_shaders/ocarina_geometry_vert_spv.h"
#include "video_core/host_shaders/ocarina_geometry_frag_spv.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
constexpr VkFormat ColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;
struct Projection {
    std::array<float, 2> old_sign, new_sign, size;
    u32 negative_one_to_one, coverage_tag;
};
constexpr auto Stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
} // namespace

OcarinaGeometryPass::OcarinaGeometryPass(const Device& device, MemoryAllocator& allocator, VkExtent2D size)
    : extent{size}, color{CreateWrappedImage(allocator, size, ColorFormat)},
      depth{allocator.CreateImage(VkImageCreateInfo{
          .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
          .format = DepthFormat, .extent = {size.width,size.height,1}, .mipLevels=1, .arrayLayers=1,
          .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
          .usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT})},
      color_view{CreateWrappedImageView(device,color,ColorFormat)},
      depth_view{device.GetLogical().CreateImageView(VkImageViewCreateInfo{
          .sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=*depth,
          .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=DepthFormat,
          .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}})} {
    const auto attachment = [](VkFormat format, VkAttachmentStoreOp store, VkImageLayout final_layout) {
        return VkAttachmentDescription{.format=format, .samples=VK_SAMPLE_COUNT_1_BIT,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp=store,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout=final_layout};
    };
    const std::array attachments{
        attachment(ColorFormat,VK_ATTACHMENT_STORE_OP_STORE,VK_IMAGE_LAYOUT_GENERAL),
        attachment(DepthFormat,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)};
    constexpr VkAttachmentReference color_ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    constexpr VkAttachmentReference depth_ref{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&color_ref,.pDepthStencilAttachment=&depth_ref};
    constexpr auto write_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr auto writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const std::array dependencies{
        VkSubpassDependency{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,write_stages,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,writes | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,0},
        VkSubpassDependency{0,VK_SUBPASS_EXTERNAL,write_stages,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            writes,VK_ACCESS_SHADER_READ_BIT,0}};
    const auto& gpu = device.GetLogical();
    pass=gpu.CreateRenderPass(VkRenderPassCreateInfo{.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=2,.pAttachments=attachments.data(),.subpassCount=1,.pSubpasses=&subpass,
        .dependencyCount=2,.pDependencies=dependencies.data()});
    const std::array views{*color_view,*depth_view};
    framebuffer=gpu.CreateFramebuffer(VkFramebufferCreateInfo{.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=*pass,.attachmentCount=2,.pAttachments=views.data(),.width=size.width,.height=size.height,.layers=1});
    constexpr VkPushConstantRange constants{Stages,0,sizeof(Projection)};
    layout=gpu.CreatePipelineLayout(VkPipelineLayoutCreateInfo{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount=1,.pPushConstantRanges=&constants});
    // Each transform feedback vertex contains float4 clip position and uint ID at byte 16.
    // Instancing exposes all three IDs to the shader for complete-triangle validation.
    const std::array bindings{VkVertexInputBindingDescription{0,96,VK_VERTEX_INPUT_RATE_INSTANCE},
                              VkVertexInputBindingDescription{1,96,VK_VERTEX_INPUT_RATE_INSTANCE}};
    std::array<VkVertexInputAttributeDescription,12> attributes{};
    for (u32 i=0; i<3; ++i) {
        attributes[i]   ={i,0,VK_FORMAT_R32G32B32A32_SFLOAT,32*i};
        attributes[i+3] ={i+3,1,VK_FORMAT_R32G32B32A32_SFLOAT,32*i};
        attributes[i+6] ={i+6,0,VK_FORMAT_R32_UINT,32*i+16};
        attributes[i+9] ={i+9,1,VK_FORMAT_R32_UINT,32*i+16};
    }
    const VkPipelineVertexInputStateCreateInfo input{.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=2,.pVertexBindingDescriptions=bindings.data(),
        .vertexAttributeDescriptionCount=12,.pVertexAttributeDescriptions=attributes.data()};
    const VkPipelineDepthStencilStateCreateInfo depth_state{.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=VK_TRUE,.depthWriteEnable=VK_TRUE,.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL};
    auto vs=CreateWrappedShaderModule(device,OCARINA_GEOMETRY_VERT_SPV);
    auto fs=CreateWrappedShaderModule(device,OCARINA_GEOMETRY_FRAG_SPV);
    pipeline=CreateWrappedPipeline(device,pass,layout,{vs,fs},&input,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,&depth_state);
}

VkImageView OcarinaGeometryPass::Render(Scheduler& scheduler, std::span<const OcarinaGeometryDraw> draws) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, draws=std::vector<OcarinaGeometryDraw>(draws.begin(),draws.end())](vk::CommandBuffer cmd) {
        const VkMemoryBarrier ready{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,ready);
        const std::array clears{VkClearValue{.color={}},VkClearValue{.depthStencil={1.f,0}}};
        const VkRect2D area{{0,0},extent};
        cmd.BeginRenderPass(VkRenderPassBeginInfo{.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass=*pass,.framebuffer=*framebuffer,.renderArea=area,.clearValueCount=2,.pClearValues=clears.data()},
            VK_SUBPASS_CONTENTS_INLINE);
        const float w=static_cast<float>(extent.width), h=static_cast<float>(extent.height);
        cmd.SetViewport(0,VkViewport{0,0,w,h,0,1});
        cmd.SetScissor(0,area);
        cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,*pipeline);
        for (const auto& draw : draws) {
            const Projection projection{draw.previous_sign,draw.current_sign,{w,h},draw.neg_one_to_one,1u + static_cast<u32>(draw.kind)};
            const std::array buffers{draw.current,draw.previous};
            const std::array offsets{draw.current_offset,draw.previous_offset};
            cmd.BindVertexBuffers(0,2,buffers.data(),offsets.data());
            cmd.PushConstants(*layout,Stages,0,sizeof(projection),&projection);
            cmd.Draw(3,draw.vertex_count/3,0,0);
        }
        cmd.EndRenderPass();
    });
    scheduler.InvalidateState();
    return *color_view;
}
} // namespace Vulkan
