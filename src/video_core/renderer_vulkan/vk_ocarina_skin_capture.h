// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <span>
#include "video_core/renderer_vulkan/vk_ocarina_camera.h"
#include <vector>
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_ocarina_geometry.h"

namespace Vulkan {
class Scheduler;
class MemoryAllocator;

class OcarinaSkinCapture {
public:
    using Maxwell = Tegra::Engines::Maxwell3D;
    struct Binding {
        VkBuffer buffer{};
        VkDeviceSize offset{}, bytes{};
    };

    OcarinaSkinCapture(MemoryAllocator& allocator, Scheduler& scheduler);
    Binding Reserve(u64 frame, const Maxwell& engine, const std::array<u64, 6>& shaders,
                    bool neg_one_to_one, const OcarinaCamera& camera, OcarinaGeometryKind kind,
                    std::optional<u64> rigid_transform);
    std::span<const OcarinaGeometryDraw> Pairs(u64 frame, const std::optional<OcarinaCamera>& camera);

private:
    struct Key {
        u64 vs{}, ps{}, index{}, index_end{}, depth{};
        u32 first{}, count{}, base_vertex{}, base_instance{}, format{};
        std::array<u32, 32> attributes{};
        std::array<std::array<u64, 3>, 32> streams{};
        auto operator<=>(const Key&) const = default;
    };
    struct Entry {
        VkDeviceSize offset{};
        OcarinaCamera camera{};
        bool neg_one_to_one{}, ambiguous{}, main_view{};
        OcarinaGeometryKind kind{};
    };
    struct Observation {
        u64 transform{};
        bool moving{}, ambiguous{};
    };
    struct Slot {
        vk::Buffer buffer;
        std::map<Key, Entry> entries;
        std::map<Key, Observation> observations;
        std::array<VkDeviceSize, 3> used{};
        std::array<u32, 3> draws{};
    };
    Scheduler& scheduler;
    std::array<Slot, 2> slots;
    u64 last_frame{~u64{}};
    u32 current{}, reports{}, duplicates{}, limited{};
    std::vector<OcarinaGeometryDraw> pairs;
};
} // namespace Vulkan
