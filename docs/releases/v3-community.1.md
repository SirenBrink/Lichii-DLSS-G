# Lichii v3 Community.1

This Windows x64 release starts at [Frizti v3](https://github.com/Frizti/Lichii/releases/tag/frizti-v3), commit `72e5ad6c22`. It uses Clang, the v3 optimization profile and ThinLTO. The emulator keeps Frizti’s 30/60 FPS choice. A new cache namespace lets this release and previous versions build and reuse their own shader caches.

## Ocarina resolution addons

Open Ocarina’s **Properties → Add-Ons**, enable **Max Resolution (built-in)** and select a **Max Resolution preset (docked)**. Choose one preset and restart the game. The default keeps the game’s output dimensions. The presets apply to the docked Ocarina 1.1.1 executable whose build ID starts with `e0f3ccfa`. The emulator verifies the original game archives and patch locations before applying the override in memory.

| Aspect | Available output sizes |
| --- | --- |
| 16:9 | 1280×720, 1600×900, 1920×1080, 2560×1440, 3840×2160 |
| 16:10 | 1280×800, 1440×900, 1680×1050, 1920×1200, 2560×1600, 3840×2400 |

The 16:10 presets change the guest display targets and camera aspect. Menus, movies and HUD elements retain their game-defined layouts. Use emulator resolution **1×** for the listed pixel dimensions. Higher multipliers further scale eligible render targets.

## NVIDIA super resolution and frame generation

1. Close Lichii and run **Install NVIDIA DLSS.cmd** in the extracted Windows release folder. It obtains the signed Streamline runtime from NVIDIA.
2. Select **Vulkan**, enable **Use NVIDIA DLSS in place of a game's DLSS (experimental)** in Graphics, then select **Ocarina of Time DLSS quality**. Enable the Max Resolution add-on for the fixed input scales.
3. To use NVIDIA frame generation on supported hardware, enable **Ocarina of Time NVIDIA Frame Generation** and restart the game. The renderer uses the hardware-reported multiplier, up to 4×. The NVIDIA option uses tagged Ocarina scene frames, the reconstructed camera motion and geometry motion. Menus, video and scene changes can return to real frames while valid inputs become available.

| Quality choice | Input for a 3840×2160 output |
| --- | --- |
| Automatic | Uses the game’s render size |
| Quality | 2560×1440 |
| Balanced | 2240×1260 |
| DLAA | 3840×2160 |

Guest alignment can round the input at other sizes. The log reports the dimensions actually evaluated. The frame-generation log reports the **measured** ratio of NVIDIA-reported displayed frames to real presents across a consecutive sample. A requested multiplier, an SR evaluation and an increased FPS counter each measure different things. The Kokiri stair shimmer remains a known visual issue.

## Verification and known behavior

On an RTX 5070 Ti in the Ocarina gameplay test, the release-candidate renderer evaluated Balanced at 2240×1260 to 3840×2160. NVIDIA reported 480 displayed frames over 120 real presents in four consecutive samples. Earlier samples measured 1.23× and 3.90×. Displayed FPS still depends on real FPS, focus, refresh rate and pacing. An offscreen synthetic test on the same GPU performed genuine SR but measured 1.00× frame generation.

All eleven output presets were checked with the supported game assets; all 44 docked preset/quality combinations passed the patch tests. The handheld profile retained its original dimensions in all eleven cases. Gameplay verification covered the 4K Balanced configuration. GPU Unswizzle is off by default. A tester reported HUD corruption with it enabled.

## Community changes and provenance

The source survey covered public branches and pull requests in the Lichii and Eglantine fork networks on November 7, 2026. The reviewed primary tips were [Lichii 222877d](https://github.com/zortos293/Lichii/commit/222877d4f7c8945b2f31a87b76d09f0a92e4f1b8) and [Eglantine 0857e8b](https://github.com/Djihads80/Eglantine/commit/0857e8b4a792ac99b87af6bb6f329127aa7df683). This branch carries compatible changes from those histories and selected fork PRs. Overlapping experimental schedulers, driver-specific visual bypasses and third-party shader-pack imports were left out.

- Corrected Catmull-Rom pixel weights on the shader bicubic path and integrated the covered texels for large reductions, including 4K to a 720p window.
- Added native cooperative matrix execution for supported hardware, with a device self-test and software fallback. Added selectable bounded waits for unbuilt pipelines while retaining the v3 default. [Lichii #475](https://github.com/zortos293/Lichii/pull/475) · [Lichii #465](https://github.com/zortos293/Lichii/pull/465)
- Integrated shader descriptor, foliage, shadow, fragment-channel and Vulkan object-lifetime fixes. [#410](https://github.com/zortos293/Lichii/pull/410) · [#353](https://github.com/zortos293/Lichii/pull/353) · [#354](https://github.com/zortos293/Lichii/pull/354) · [#434](https://github.com/zortos293/Lichii/pull/434)
- Improved buffer and texture tracking, reduced redundant page walks and fixed a Windows race when clearing page tables. [#463](https://github.com/zortos293/Lichii/pull/463) · [#478](https://github.com/zortos293/Lichii/pull/478) · [#481](https://github.com/zortos293/Lichii/pull/481) · [#485](https://github.com/zortos293/Lichii/pull/485)
- Ported scheduler and fence fixes, handling for missing mod directories, reusable syncpoint callbacks and long occlusion counts. [Eglantine #727](https://github.com/Djihads80/Eglantine/pull/727) · [Eglantine #728](https://github.com/Djihads80/Eglantine/pull/728) · [Primrose #2](https://github.com/ZnOw01/Primrose/pull/2) · [Hydro74000’s commit](https://github.com/Hydro74000/Edlws/commit/3765d4691ffe281fe70b39c0d505b3e769e2b3a9)
- Included AV1 decoding and its Windows pixel-verification test. Built-in FPS mod credit remains with Dark (Eglantine Dev). Original contributors and upstream revision references are recorded in Git.

See the [Windows build instructions](../community-windows-release.md) to rebuild the executable and run the release tests.
