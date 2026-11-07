// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_ocarina_skin_capture.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
constexpr VkDeviceSize CharacterCapacity = 8 * 1024 * 1024;
constexpr VkDeviceSize EquipmentCapacity = 2 * 1024 * 1024;
constexpr VkDeviceSize OtherCapacity = 4 * 1024 * 1024;
constexpr VkDeviceSize Capacity = CharacterCapacity + EquipmentCapacity + OtherCapacity;
constexpr std::array<VkDeviceSize, 3> Offsets{0, CharacterCapacity, CharacterCapacity + EquipmentCapacity};
constexpr std::array<VkDeviceSize, 3> Budgets{CharacterCapacity, EquipmentCapacity, OtherCapacity};
constexpr std::array<u32, 3> DrawLimits{128, 32, 64};
constexpr VkDeviceSize Stride = 32;
}

OcarinaSkinCapture::OcarinaSkinCapture(MemoryAllocator& allocator, Scheduler& scheduler_)
    : scheduler{scheduler_} {
    for (auto& slot : slots) {
        slot.buffer = allocator.CreateBuffer({
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = Capacity,
            .usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        }, MemoryUsage::DeviceLocal);
    }
}

OcarinaSkinCapture::Binding OcarinaSkinCapture::Reserve(
    u64 frame, const Maxwell& engine, const std::array<u64, 6>& shaders, bool neg_one_to_one,
    const OcarinaCamera& camera, OcarinaGeometryKind kind, std::optional<u64> rigid_transform) {
    if (frame != last_frame) {
        if (frame != last_frame + 1) {
            slots[current].entries.clear();
            slots[current].observations.clear();
        }
        last_frame = frame;
        current ^= 1;
        auto& slot = slots[current];
        slot.entries.clear();
        slot.observations.clear();
        slot.used = {};
        slot.draws = {};
        duplicates = limited = 0;
        // A skipped draw or discarded primitive must leave an invalid W, never
        // positions from two frames ago. All reads and writes are on the graphics queue.
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([buffer = *slot.buffer](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT});
            cmd.FillBuffer(buffer, 0, Capacity, 0);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT});
        });
    }
    const auto& regs = engine.regs;
    const auto& draw = engine.draw_manager.draw_state;
    Key key{.vs = shaders[1], .ps = shaders[5], .index = draw.index_buffer.StartAddress(),
            .index_end = draw.index_buffer.EndAddress(), .depth = regs.zeta.Address(),
            .first = draw.index_buffer.first, .count = draw.index_buffer.count,
            .base_vertex = draw.base_index, .base_instance = draw.base_instance,
            .format = static_cast<u32>(draw.index_buffer.format)};
    for (size_t i = 0; i < key.attributes.size(); ++i) {
        key.attributes[i] = regs.vertex_attrib_format[i].hex;
        const auto& stream = regs.vertex_streams[i];
        if (stream.IsEnabled()) {
            key.streams[i] = {stream.Address(), regs.vertex_stream_limits[i].Address(),
                              (u64{stream.frequency} << 32) | stream.stride.Value() |
                              (u64{regs.vertex_stream_instances.IsInstancingEnabled(i)} << 31)};
        }
    }
    auto& slot = slots[current];
    if (auto it = slot.entries.find(key); it != slot.entries.end()) {
        it->second.ambiguous = true;
        if (auto observed = slot.observations.find(key); observed != slot.observations.end()) {
            observed->second.ambiguous = true;
        }
        ++duplicates;
        return {};
    }
    // For ordinary rigid objects only spend capture bandwidth after their own
    // model transform changes. Keep one settling frame for interpolation.
    if (rigid_transform) {
        const auto& prior = slots[current ^ 1].observations;
        const auto old = prior.find(key);
        const bool moving = old != prior.end() && !old->second.ambiguous &&
                            old->second.transform != *rigid_transform;
        if (slot.observations.size() >= 512) {
            return {};
        }
        auto [it, inserted] = slot.observations.emplace(key, Observation{*rigid_transform, moving});
        if (!inserted) {
            it->second.ambiguous = true;
            return {};
        }
        if (old == prior.end() || old->second.ambiguous || (!moving && !old->second.moving)) {
            return {};
        }
    }
    const VkDeviceSize bytes = Stride * key.count;
    const auto group = static_cast<size_t>(kind);
    auto& used = slot.used[group];
    const auto relative_offset = Common::AlignUp(used, VkDeviceSize{256});
    if (slot.draws[group] >= DrawLimits[group] || bytes > Budgets[group] - relative_offset) {
        ++limited;
        return {};
    }
    const auto offset = relative_offset + Offsets[group];
    slot.entries.emplace(key, Entry{.offset=offset, .camera=camera, .neg_one_to_one=neg_one_to_one, .kind=kind});
    ++slot.draws[group];
    used = relative_offset + bytes;
    return {*slot.buffer, offset, bytes};
}

std::span<const OcarinaGeometryDraw> OcarinaSkinCapture::Pairs(
    u64 frame, const std::optional<OcarinaCamera>& camera) {
    pairs.clear();
    if (frame != last_frame) {
        return {};
    }
    const auto& previous = slots[current ^ 1];
    auto& now = slots[current];
    u32 other_camera{};
    std::array<u32, 3> matched{};
    for (auto& [key, entry] : now.entries) {
        const auto same = [](float a, float b) { return std::abs(a - b) < 0.0001f; };
        entry.main_view = camera && std::ranges::equal(camera->view, entry.camera.view, same) &&
                          std::ranges::equal(camera->projection, entry.camera.projection, same);
        if (!entry.main_view) {
            ++other_camera;
            continue;
        }
        const auto old = previous.entries.find(key);
        if (entry.ambiguous || old == previous.entries.end() || old->second.ambiguous ||
            !old->second.main_view || old->second.kind != entry.kind ||
            old->second.neg_one_to_one != entry.neg_one_to_one) {
            continue;
        }
        pairs.push_back({*previous.buffer, *now.buffer, old->second.offset, entry.offset,
                         key.count, entry.neg_one_to_one, old->second.camera.viewport_sign,
                         entry.camera.viewport_sign, entry.kind});
        ++matched[static_cast<size_t>(entry.kind)];
    }
    if (reports++ < 8 || reports % 180 == 0) {
        LOG_INFO(Render_Vulkan, "Ocarina geometry frame {}: {} captured draws, {} matched, "
                 "{} wrong-view, {} duplicate keys, {} over budget, {} bytes; equipment {}/{} matched, other {}/{} matched",
                 frame, now.entries.size(), pairs.size(), other_camera, duplicates, limited,
                 now.used[0] + now.used[1] + now.used[2], matched[1], now.draws[1],
                 matched[2], now.draws[2]);
    }
    return pairs;
}
} // namespace Vulkan
