// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {
inline constexpr u64 OcarinaTitleId = 0x0400486024DEC000ULL;
struct OutputSize {
    u32 width;
    u32 height;
};
// The index is persisted in per-game configuration. Append new sizes to preserve saved choices.
inline constexpr std::array<OutputSize, 12> OcarinaOutputSizes{{
    {0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
    {1280, 800}, {1920, 1200}, {2560, 1600}, {3840, 2400},
    {1440, 900}, {1680, 1050},
}};

struct OcarinaResolutionAddon {
    OutputSize output{};
    VirtualDir overlay;
    bool applied{};
    void Prepare(const VirtualDir& romfs);
    // Called only after the title, build ID and Max Resolution switch have been checked.
    bool PatchModule(std::span<const u8> original, std::span<u8> patched, float input_scale);
};
} // namespace FileSys
