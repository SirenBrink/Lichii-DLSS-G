// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <algorithm>
#include <bit>
#include <vector>
#include <cstring>
#include <memory>
#include <span>

#include <openssl/evp.h>
#include <zstd.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/file_sys/ocarina_resolution.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {
namespace {

// The offsets below refer to these exact Ocarina 1.1.1 archives.
std::vector<u8> ReadVerified(const VirtualDir& pack, std::string_view name, size_t size,
                             std::string_view sha256) {
    const auto file = pack->GetFile(name);
    if (!file || file->GetSize() != size) {
        return {};
    }
    auto bytes = file->ReadAllBytes();
    std::array<u8, 32> digest;
    unsigned int length{};
    if (bytes.size() != size ||
        !EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) ||
        length != digest.size() || Common::HexToString(digest, false) != sha256) {
        return {};
    }
    return bytes;
}

constexpr char BootPack[] = "Bootup.Nin_Ounce_NVN2.pack.zs";

VirtualFile MakeDockedProfile(const VirtualDir& pack, OutputSize output) {
    auto boot = ReadVerified(pack, BootPack, 117719,
                             "107aeccfd860939371841db696ed278f2bf30d941789bd5cfb91a41f48b83362");
    auto dictionaries = ReadVerified(pack, "ZsDic.pack.zs", 132490,
                                     "1f35801d24503905093bb2f6634e5779c1413ca7e6756421160e44d996e60904");
    if (boot.empty() || dictionaries.empty()) {
        return {};
    }
    std::vector<u8> dictionary_pack(524448);
    if (ZSTD_decompress(dictionary_pack.data(), dictionary_pack.size(), dictionaries.data(),
                        dictionaries.size()) != dictionary_pack.size()) {
        return {};
    }
    const auto dictionary = std::span<const u8>{dictionary_pack}.subspan(262304, 131072);
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decoder{ZSTD_createDCtx(), ZSTD_freeDCtx};
    std::vector<u8> sarc(675656);
    if (!decoder || ZSTD_decompress_usingDict(decoder.get(), sarc.data(), sarc.size(), boot.data(),
                                              boot.size(), dictionary.data(), dictionary.size()) != sarc.size()) {
        return {};
    }

    // Both docked profiles share one BYML vector for BaseResolution and FinalImageResolution.
    // The handheld profile references a separate vector in the same ModelSceneExtensionParam.
    constexpr size_t model_scene = 27136;
    const u32_le width{output.width};
    const u32_le height{output.height};
    std::memcpy(sarc.data() + model_scene + 0xdbc, &width, sizeof(width));
    std::memcpy(sarc.data() + model_scene + 0xdc4, &height, sizeof(height));

    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> encoder{ZSTD_createCCtx(), ZSTD_freeCCtx};
    if (!encoder) {
        return {};
    }
    boot.resize(ZSTD_compressBound(sarc.size()));
    const size_t size = ZSTD_compress_usingDict(encoder.get(), boot.data(), boot.size(), sarc.data(),
                                                sarc.size(), dictionary.data(), dictionary.size(), 14);
    if (ZSTD_isError(size)) {
        return {};
    }
    boot.resize(size);
    return std::make_shared<VectorVfsFile>(std::move(boot), BootPack);
}

VirtualFile MakeTexturePool(const VirtualDir& heap_directory, OutputSize output) {
    constexpr char filename[] = "Product.Nin_Ounce_NVN2.HeapDef.byml.zs";
    auto compressed = ReadVerified(heap_directory, filename, 1474,
                                   "bddf27eecf45a15b8448dea5720245d184271a3ca8e94a451386ce05b86f6a25");
    std::array<u8, 3596> byml{};
    if (compressed.empty() || ZSTD_decompress(byml.data(), byml.size(), compressed.data(),
                                              compressed.size()) != byml.size()) {
        return {};
    }

    // The guest reads Size in KiB. Scale the texture pool by the pixel count and give
    // its parent the same increase, preserving space for its other allocations.
    constexpr size_t texture_offset = 0xa54;
    constexpr size_t graphics_offset = 0xaf8;
    constexpr u32 texture_kib = 312320;
    constexpr u32 graphics_kib = 861184;
    u32_le texture{}, graphics{};
    std::memcpy(&texture, byml.data() + texture_offset, sizeof(texture));
    std::memcpy(&graphics, byml.data() + graphics_offset, sizeof(graphics));
    if (texture != texture_kib || graphics != graphics_kib) {
        return {};
    }
    const u64 pixels = u64{output.width} * output.height;
    constexpr u64 base_pixels = 2560ULL * 1440;
    const u32 native_texture_kib = static_cast<u32>((texture_kib * pixels + base_pixels - 1) / base_pixels);
    texture = native_texture_kib;
    graphics = graphics_kib + (native_texture_kib - texture_kib);
    std::memcpy(byml.data() + texture_offset, &texture, sizeof(texture));
    std::memcpy(byml.data() + graphics_offset, &graphics, sizeof(graphics));

    compressed.resize(ZSTD_compressBound(byml.size()));
    const size_t size = ZSTD_compress(compressed.data(), compressed.size(), byml.data(),
                                      byml.size(), 14);
    if (ZSTD_isError(size)) {
        return {};
    }
    compressed.resize(size);
    return std::make_shared<VectorVfsFile>(std::move(compressed), filename);
}

} // namespace

void OcarinaResolutionAddon::Prepare(const VirtualDir& romfs) {
    const auto index = Settings::values.ocarina_output_preset.GetValue();
    if (!romfs || !Settings::IsDockedMode() || !index || index >= OcarinaOutputSizes.size()) {
        return;
    }
    const auto size = OcarinaOutputSizes[index];
    const auto pack = romfs->GetSubdirectory("Pack");
    const auto boot = pack ? MakeDockedProfile(pack, size) : nullptr;
    if (!boot) {
        LOG_WARNING(Loader, "Ocarina: resolution addon could not verify the docked profile");
        return;
    }
    auto patched_pack = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{boot}, std::vector<VirtualDir>{}, "Pack");
    auto root = std::make_shared<VectorVfsDirectory>(std::vector<VirtualFile>{},
                                                    std::vector<VirtualDir>{patched_pack});
    if (u64{size.width} * size.height > 2560ULL * 1440) {
        const auto heaps = romfs->GetDirectoryRelative("System/HeapSize");
        const auto pool = heaps ? MakeTexturePool(heaps, size) : nullptr;
        if (!pool) {
            LOG_WARNING(Loader, "Ocarina: resolution addon could not verify the texture heap");
            return;
        }
        auto patched_heaps = std::make_shared<VectorVfsDirectory>(
            std::vector<VirtualFile>{pool}, std::vector<VirtualDir>{}, "HeapSize");
        root->AddDirectory(std::make_shared<VectorVfsDirectory>(
            std::vector<VirtualFile>{}, std::vector<VirtualDir>{patched_heaps}, "System"));
    }
    output = size;
    overlay = std::move(root);
}

bool OcarinaResolutionAddon::PatchModule(std::span<const u8> original, std::span<u8> patched,
                                         float input_scale) {
    if (!Settings::IsDockedMode() || (!overlay && input_scale == 0.f)) {
        return false;
    }
    if (Settings::values.ocarina_output_preset.GetValue() && !overlay) {
        return false;
    }
    struct Word { size_t offset; u32 expected; u32 value; };
    std::vector<Word> words;
    if (overlay) {
        const u32 width = output.width;
        const u32 height = output.height;
        words = {{0x3f58568, 2560, width}, {0x3f5856c, 1440, height},
                 {0x1202ee0, 0xd2814008, 0xd2800008 | (width << 5)},
                 {0x1202ee4, 0xf2c0b408, 0xf2c00008 | (height << 5)},
                 {0x31f39f4, 0xd2814008, 0xd2800008 | (width << 5)},
                 {0x31f39f8, 0xf2c0b408, 0xf2c00008 | (height << 5)}};
        if (output.width * 10 == output.height * 16) {
            words.push_back({0x3e4131c, 0x3fe38e39, std::bit_cast<u32>(16.f / 10.f)});
        }
    }
    if (input_scale > 0.f) {
        const u32 bits = std::bit_cast<u32>(input_scale);
        words.insert(words.end(), {{0x806418, 0xd2c00028, 0x52800009 | ((bits & 0xffff) << 5)},
                                   {0x80641c, 0x9a888128, 0xaa1f03e8},
                                   {0x806420, 0x52a7f009, 0x72a00009 | ((bits >> 16) << 5)},
                                   {0x31d4014, 0x1a9f050b, 0x5280002b}});
    }
    for (const auto& word : words) {
        u32_le expected{word.expected};
        if (original.size() < word.offset + 4 || patched.size() < word.offset + 4 ||
            std::memcmp(original.data() + word.offset, &expected, 4) ||
            std::memcmp(patched.data() + word.offset, &expected, 4)) {
            LOG_WARNING(Loader, "Ocarina: incompatible resolution patch at main+{:#x}", word.offset);
            return false;
        }
    }
    for (const auto& word : words) {
        const u32_le value{word.value};
        std::memcpy(patched.data() + word.offset, &value, 4);
    }
    applied = true;
    LOG_INFO(Loader, "Ocarina: docked output {}x{}, requested input scale {}",
             overlay ? output.width : 2560, overlay ? output.height : 1440, input_scale);
    return true;
}
} // namespace FileSys
