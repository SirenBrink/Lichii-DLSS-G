// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCore::Nx2Debug {
enum class OcarinaMode { Dlss, Guest, Packed, NoDetail };
inline std::atomic<OcarinaMode> ocarina_mode{OcarinaMode::Dlss};
inline std::atomic<bool> ocarina_sr_geometry{false};
inline std::atomic<bool> ocarina_present_generated{true};
inline std::atomic<u64> ocarina_evaluations{};
inline std::atomic<u64> ocarina_scene_resolves{};
} // namespace VideoCore::Nx2Debug
