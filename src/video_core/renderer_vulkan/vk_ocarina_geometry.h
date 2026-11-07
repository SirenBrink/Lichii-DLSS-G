// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
enum class OcarinaGeometryKind : u32 { Character, Equipment, Other };

struct OcarinaGeometryDraw {
    VkBuffer previous, current;
    VkDeviceSize previous_offset, current_offset;
    u32 vertex_count;
    bool neg_one_to_one;
    std::array<float, 2> previous_sign, current_sign;
    OcarinaGeometryKind kind{};
};
} // namespace Vulkan
