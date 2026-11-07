# Windows x64 community release

Open an x64 Visual Studio Developer PowerShell with the Desktop C++ workload, Windows SDK, Ninja and CMake available. Use LLVM 24 with clang-cl, lld-link and llvm-lib from the same distribution. The release build used LLVM 24.0.0git at LLVM commit 510126255f89d693717c1ce7105b593f995f07c1, the MSVC 14.51.36231 x64 libraries and the Switch 2 profile in this repository.

From a checkout of the release tag, run:

```powershell
$src = (Resolve-Path .).Path.Replace('\','/')
$llvm = (Split-Path (Get-Command clang-cl.exe -ErrorAction Stop).Source).Replace('\','/')
$profile = "$src/eglantine.profdata"
$flags = '/O2 /DNDEBUG /clang:-flto=thin "/clang:-fprofile-use=' + $profile + '" /clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date /clang:-Wno-backend-plugin'
cmake -S . -B build/community -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe" `
    "-DCMAKE_LINKER=$llvm/lld-link.exe" "-DCMAKE_AR=$llvm/llvm-lib.exe" `
    "-DCMAKE_C_FLAGS_RELEASE=$flags" "-DCMAKE_CXX_FLAGS_RELEASE=$flags" `
    -DLICHII_BUILD_TAG=v3-community.1 -DLICHII_BUILD_PLATFORM=windows-x86_64-pgo `
    -DENABLE_NVIDIA_DLSS=ON -DYUZU_CMD=ON -DYUZU_TESTS=ON -DLICHII_AV1_SMOKE_TEST=ON `
    -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_UPDATE_CHECKER=OFF `
    -DUSE_DISCORD_PRESENCE=OFF -DENABLE_QT_TRANSLATION=OFF
cmake --build build/community --target yuzu yuzu-cmd tests --parallel 12
```

Install a Vulkan SDK with glslangValidator available or set `-DGLSLANGVALIDATOR=<path>` when configuring. CMake downloads the dependency versions recorded in `cpmfile.json`. For an offline build, populate the dependency cache first and provide the Streamline v2.14.16 include folder's parent with `-DEGLANTINE_STREAMLINE_SDK=<path>`. The resulting Windows executables and graphics runtime installer are in `build/community/bin`. The Visual C++ x64 redistributable is required on the user's PC.

Run the test executable in a disposable Windows profile or with APPDATA, LOCALAPPDATA, TEMP and TMP pointing to dedicated test directories. Motion quality, frame generation and game compatibility require a separate test on a supported GPU.

The optional runtime installer reads the production DLLs and licenses from [NVIDIA's official v2.14.16 release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16), checks the published SHA-256 and checks each DLL's NVIDIA Authenticode signature. The public source and Windows release include the integration and installer. NVIDIA supplies the model and plugin binaries directly when the user runs the installer.
