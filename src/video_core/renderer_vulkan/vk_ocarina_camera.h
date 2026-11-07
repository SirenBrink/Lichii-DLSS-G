// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace Vulkan {

// Ocarina 1.1.1's main-view VS constant buffer. The guest uses column vectors.
struct OcarinaCamera {
    std::array<float, 12> view;
    std::array<float, 16> projection;
    // Direction of increasing framebuffer pixels relative to guest clip X/Y.
    std::array<float, 2> viewport_sign{1.f, -1.f};
    std::array<float, 16> ClipToPrevious(const OcarinaCamera& previous) const;

    bool MatchesScene(float aspect) const {
        const auto finite = [](float v) { return std::isfinite(v); };
        if (!std::ranges::all_of(view, finite) || !std::ranges::all_of(projection, finite)) {
            return false;
        }
        for (size_t row = 0; row < 3; ++row) {
            for (size_t other = row; other < 3; ++other) {
                float dot{};
                for (size_t col = 0; col < 3; ++col) {
                    dot += view[row * 4 + col] * view[other * 4 + col];
                }
                if (std::abs(dot - float(row == other)) > 0.005f) {
                    return false;
                }
            }
        }
        const auto& p = projection;
        for (const auto i : {1, 2, 3, 4, 6, 7, 8, 9, 12, 13, 15}) {
            if (std::abs(p[i]) > 0.00001f) {
                return false;
            }
        }
        return p[0] > 0.f && p[5] > 0.f && std::abs(p[5] / p[0] - aspect) < 0.02f &&
               p[10] < -1.f && p[11] < 0.f && p[14] == -1.f;
    }
};

} // namespace Vulkan
