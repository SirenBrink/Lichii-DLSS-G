// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/ocarina_dlss_debug.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"

#include <sl_security.h>

#include <softpub.h>
#include <wintrust.h>

namespace Vulkan::Streamline {
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct ScenePresentation {
    u32 width, height, left, top, view_width, view_height, scene_width, scene_height;
    bool operator==(const ScenePresentation&) const = default;
};

struct Generation {
    PFun_slDLSSGGetState* get_state{};
    PFun_slDLSSGSetOptions* set_options{};
    PFun_slReflexSetOptions* reflex_options{};
    PFun_slReflexSleep* sleep{};
    PFun_slPCLSetMarker* marker{};
    sl::FrameToken* token{};
    u32 tagged_width{}, tagged_height{}, generation_count{}, min_dimension{};
    std::optional<ScenePresentation> pending_scene, last_presented_scene;
    u32 last_presented_token{};
    u32 window_real{}, window_displayed{}, last_transient_status{};
    bool prime_present_counter{};
    bool ready{}, rendering{}, presenting{}, on{}, failed{};
};

struct State {
    HMODULE module{};
    Api api{};
    bool generation_files{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    bool started{};
    bool attached{};
    PFN_vkGetInstanceProcAddr proxy_loader{};
    Generation generation{};
    std::mutex generation_mutex;
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    if (!IsSignedByNvidia(target)) {
        LOG_WARNING(Render_Vulkan, "Cached DLSS {} is not signed by NVIDIA",
                    Common::FS::PathToUTF8String(target));
        return {};
    }
    return dir;
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info)) {
        api.set_tag_for_frame =
            reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(m, "slSetTagForFrame"));
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

std::vector<std::string> Names(uint32_t count, const char** names) {
    std::vector<std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    const bool prefer_generation = FrameGenerationRequested();
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (!std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            continue;
        }
        const bool complete = std::ranges::all_of(
            std::array{"nvngx_dlss.dll", "nvngx_dlssg.dll", "sl.common.dll", "sl.dlss.dll",
                       "sl.dlss_g.dll", "sl.pcl.dll", "sl.reflex.dll", "NvLowLatencyVk.dll"},
            [&](const char* name) { return std::filesystem::is_regular_file(dir / name, ec); });
        if (files.interposer.empty() || (prefer_generation && complete)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            files.generation_files = complete;
        }
        if (!prefer_generation || complete) {
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

bool FrameGenerationRequested() {
    const char* setting = std::getenv("EDEN_OOT_DLSSG");
    const bool requested = setting ? std::string_view{setting} == "1"
                                   : Settings::values.ocarina_dlss_frame_generation.GetValue();
    return requested && Settings::values.nvidia_dlss.GetValue() &&
           Settings::GetCurrentProgramID() == 0x0400486024DEC000ULL;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (!files.model.empty() && !IsSignedByNvidia(files.model)) {
            LOG_WARNING(Render_Vulkan, "DLSS model {} is not signed by NVIDIA; using FSR 3.1",
                        Common::FS::PathToUTF8String(files.model));
            return false;
        }
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); using FSR 3.1",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.generation_files = files.generation_files;
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty()) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                    sl::kFeaturePCL};
    const bool request_generation = FrameGenerationRequested() && state.generation_files;
    if (FrameGenerationRequested() && !request_generation) {
        LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G runtime incomplete; keeping SR. Run Install NVIDIA DLSS.cmd next to the emulator.");
    }
    state.proxy_loader = nullptr;
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    const auto sr_flags = preferences.flags;
    if (request_generation) {
        // slSetTagForFrame explicitly rejects calls unless frame-based tagging was negotiated.
        // Keep all frame-generation plugins on the locally installed SDK version.
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                            sl::PreferenceFlags::eUseManualHooking |
                            sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    }
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = request_generation ? std::size(features) : 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const Api& api = state.api;
    sl::Result initialized = api.init(preferences, sl::kSDKVersion);
    if (initialized != sl::Result::eOk && request_generation) {
        LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G runtime initialization failed ({}); retrying SR",
                    static_cast<int>(initialized));
        preferences.numFeaturesToLoad = 1;
        preferences.flags = sr_flags;
        initialized = api.init(preferences, sl::kSDKVersion);
    }
    if (initialized != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); using FSR 3.1",
                  static_cast<int>(initialized));
        return false;
    }
    if (preferences.numFeaturesToLoad > 1) {
        bool ready = api.set_tag_for_frame != nullptr;
        for (const sl::Feature feature : {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL}) {
            sl::FeatureRequirements req{};
            const auto result = api.get_feature_requirements(feature, req);
            if (result != sl::Result::eOk) {
                LOG_WARNING(Render_Vulkan, "Streamline feature {} unavailable ({}); keeping DLSS SR",
                            feature, static_cast<int>(result));
                ready = false;
                break;
            }
            LOG_INFO(Render_Vulkan, "Streamline feature {} requires Vulkan queues graphics={} compute={} optical-flow={}",
                     feature, req.vkNumGraphicsQueuesRequired, req.vkNumComputeQueuesRequired,
                     req.vkNumOpticalFlowQueuesRequired);
        }
        if (ready) {
            state.proxy_loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(state.module, "vkGetInstanceProcAddr"));
        }
        if (!state.proxy_loader) {
            api.shutdown();
            preferences.numFeaturesToLoad = 1;
            preferences.flags = sr_flags;
            if (api.init(preferences, sl::kSDKVersion) != sl::Result::eOk) {
                LOG_ERROR(Render_Vulkan, "Could not restart Streamline in SR-only mode");
                return false;
            }
        }
    }
    sl::FeatureRequirements requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeatureDLSS, requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "Streamline found no DLSS plugin (result {}); put sl.dlss.dll next to "
                    "sl.interposer.dll or install the NVIDIA app. Using FSR 3.1",
                    static_cast<int>(result));
        api.shutdown();
        return false;
    }
    if (requirements.vkNumComputeQueuesRequired != 0 ||
        requirements.vkNumGraphicsQueuesRequired != 0) {
        LOG_WARNING(Render_Vulkan, "This DLSS wants extra Vulkan queues; using FSR 3.1");
        api.shutdown();
        return false;
    }
    state.instance_extensions =
        Names(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
    state.device_extensions =
        Names(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    sl::FeatureVersion version{};
    api.get_feature_version(sl::kFeatureDLSS, version);
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline {} started from {} (DLSS {})",
             version.versionSL.toStr(),
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             version.versionNGX.toStr());
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

PFN_vkGetInstanceProcAddr FrameGenerationInstanceLoader() {
    return WantsDevice() ? GetState().proxy_loader : nullptr;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, u32 present_family, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = graphics_family;
    info.graphicsQueueFamily = graphics_family;
    // When SL created the device it already assigned its dedicated queues and initialized
    // plugins. slSetVulkanInfo here would overwrite that mapping with the app's queue zero.
    if (const sl::Result result = state.proxy_loader ? sl::Result::eOk : state.api.set_vulkan_info(info);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    if (const sl::Result result = state.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is not supported on this GPU or driver (Streamline result {}); "
                    "using FSR 3.1",
                    static_cast<int>(result));
        Stop();
        return false;
    }
    // Manual hooking: Streamline does its per-frame work in these.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    if (!state.proxy_loader) {
        hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
        hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
        hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
        hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
        hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
        hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    }
    state.attached = true;
    if (state.proxy_loader && graphics_family != present_family) {
        LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G requires shared graphics/present queue for input synchronization");
    } else if (state.proxy_loader) {
        auto& gen = state.generation;
        const auto resolve_feature = [&](sl::Feature feature, const char* name, auto& dest) {
            void* ptr{};
            const auto result = state.api.get_feature_function(feature, name, ptr);
            dest = reinterpret_cast<std::remove_reference_t<decltype(dest)>>(ptr);
            return result == sl::Result::eOk && dest;
        };
        const auto supported = [&](sl::Feature feature) {
            const auto result = state.api.is_feature_supported(feature, adapter);
            LOG_INFO(Render_Vulkan, "Streamline device support feature {} result {}", feature,
                     static_cast<int>(result));
            return result == sl::Result::eOk;
        };
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        gen.ready = supported(sl::kFeatureDLSS_G) && supported(sl::kFeatureReflex) &&
                    supported(sl::kFeaturePCL) &&
                    resolve_feature(sl::kFeatureDLSS_G, "slDLSSGGetState", gen.get_state) &&
                    resolve_feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", gen.set_options) &&
                    resolve_feature(sl::kFeatureReflex, "slReflexSetOptions", gen.reflex_options) &&
                    resolve_feature(sl::kFeatureReflex, "slReflexSleep", gen.sleep) &&
                    resolve_feature(sl::kFeaturePCL, "slPCLSetMarker", gen.marker) &&
                    gen.reflex_options(reflex) == sl::Result::eOk;
        LOG_INFO(Render_Vulkan, "Ocarina DLSS Frame Generation + Reflex device ready={}", gen.ready);
    }
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through Streamline");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached;
}

namespace {
const sl::ViewportHandle GameViewport{0u};

bool GenerationResult(Generation& gen, sl::Result result, const char* operation) {
    if (result == sl::Result::eOk) {
        return true;
    }
    LOG_ERROR(Render_Vulkan, "Ocarina DLSS-G {} failed ({}); suspending generation", operation,
              static_cast<int>(result));
    gen.failed = true;
    return false;
}

bool SetGeneration(Generation& gen, bool enable) {
    if (gen.on == enable) {
        return true;
    }
    sl::DLSSGOptions options{};
    options.mode = enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    // Temporary transitions (movies, menus, and history resets) should not pay for
    // reallocating DLSS-G. The feature is released when the session shuts down.
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.numFramesToGenerate = (std::max)(1u, gen.generation_count);
    if (!GenerationResult(gen, gen.set_options(GameViewport, options), "set options")) {
        return false;
    }
    gen.on = enable;
    gen.window_real = gen.window_displayed = 0;
    gen.prime_present_counter = enable;
    LOG_INFO(Render_Vulkan, "Ocarina DLSS-G {} ({} generated per real frame)",
             enable ? "on" : "off", enable ? gen.generation_count : 0u);
    return true;
}

bool Marker(Generation& gen, sl::PCLMarker marker) {
    return GenerationResult(gen, gen.marker(marker, *gen.token), "PCL marker");
}

void StartSimulation(State& state) {
    auto& gen = state.generation;
    if (!gen.ready || gen.failed || gen.token) {
        return;
    }
    if (!GenerationResult(gen, state.api.get_new_frame_token(gen.token, nullptr), "get frame token") ||
        !gen.token || !GenerationResult(gen, gen.sleep(*gen.token), "Reflex sleep")) {
        return;
    }
    Marker(gen, sl::PCLMarker::eSimulationStart);
}
} // namespace

bool IsDlssGAvailable() {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    return state.generation.ready && !state.generation.failed;
}

void BeginFrameGenerationRender() {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    StartSimulation(state);
    if (!gen.ready || gen.failed || !gen.token || gen.rendering) {
        return;
    }
    gen.rendering = Marker(gen, sl::PCLMarker::eSimulationEnd) &&
                    Marker(gen, sl::PCLMarker::eRenderSubmitStart);
}

sl::FrameToken* GetFrameGenerationToken() {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    return gen.rendering && !gen.failed ? gen.token : nullptr;
}

void MarkFrameGenerationTagged(const sl::FrameToken& token, u32 width, u32 height) {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    if (!gen.failed && gen.rendering && gen.token == &token) {
        gen.tagged_width = width;
        gen.tagged_height = height;
    }
}

void BeginFrameGenerationPresent(u32 width, u32 height, u32 left, u32 top, u32 view_width,
                                 u32 view_height) {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    if (!gen.ready) {
        return;
    }
    gen.pending_scene.reset();
    if (gen.failed || !gen.rendering || !gen.token) {
        SetGeneration(gen, false);
        return;
    }
    gen.presenting = Marker(gen, sl::PCLMarker::eRenderSubmitEnd) &&
                     Marker(gen, sl::PCLMarker::ePresentStart);
    if (!gen.presenting || !view_width || !view_height || !gen.tagged_width || !gen.tagged_height ||
        left > width || top > height || view_width > width - left || view_height > height - top) {
        SetGeneration(gen, false);
        return;
    }
    // Allow one display pixel of rounding introduced by aspect-ratio correction.
    const u64 across = u64{view_width} * gen.tagged_height;
    const u64 down = u64{view_height} * gen.tagged_width;
    if ((across > down ? across - down : down - across) > (std::max)(gen.tagged_width, gen.tagged_height)) {
        SetGeneration(gen, false);
        return;
    }
    if (!gen.generation_count) {
        sl::DLSSGState caps{};
        if (!GenerationResult(gen, gen.get_state(GameViewport, caps, nullptr), "query capabilities") ||
            caps.numFramesToGenerateMax == 0) {
            gen.failed = true;
            LOG_WARNING(Render_Vulkan, "DLSS-G reports no usable frame multiplier");
            return;
        }
        gen.generation_count = (std::min)(3u, caps.numFramesToGenerateMax);
        gen.min_dimension = caps.minWidthOrHeight;
        LOG_INFO(Render_Vulkan, "Ocarina DLSS-G hardware max {}x, requested {}x, minimum dimension {}",
                 caps.numFramesToGenerateMax + 1, gen.generation_count + 1, gen.min_dimension);
    }
    if ((std::min)(view_width, view_height) < gen.min_dimension) {
        SetGeneration(gen, false);
        return;
    }
    const sl::Extent extent{.top = top, .left = left, .width = view_width, .height = view_height};
    const sl::ResourceTag rect{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &extent};
    if (!GenerationResult(gen, state.api.set_tag_for_frame(*gen.token, GameViewport, &rect, 1, nullptr),
                          "tag final backbuffer region")) {
        SetGeneration(gen, false);
        return;
    }
    if (!VideoCore::Nx2Debug::ocarina_present_generated.load(std::memory_order_relaxed)) {
        SetGeneration(gen, false);
        return;
    }
    // An evaluation alone does not mean its backbuffer reached the display.
    // Present one real frame first when returning from menus, after a failed
    // present, or when the previous backbuffer used a different scene region.
    gen.pending_scene.emplace(ScenePresentation{width, height, left, top, view_width, view_height,
                                                gen.tagged_width, gen.tagged_height});
    SetGeneration(gen, gen.pending_scene == gen.last_presented_scene &&
                       u32(*gen.token) == gen.last_presented_token + 1u);
}

void EndFrameGenerationPresent(bool present_succeeded) {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    if (!gen.ready) {
        return;
    }
    if (gen.presenting && gen.token) {
        Marker(gen, sl::PCLMarker::ePresentEnd);
    }
    bool valid_scene = present_succeeded && !gen.failed;
    if (gen.on && valid_scene) {
        sl::DLSSGState result{};
        if (!GenerationResult(gen, gen.get_state(GameViewport, result, nullptr), "get present stats")) {
            valid_scene = false;
        } else if (result.status != sl::DLSSGStatus::eOk) {
            const auto status = static_cast<u32>(result.status);
            // Resizing and rebuilding scene history can temporarily invalidate a frame. Let
            // the next pair of contiguous, validated inputs qualify again.
            constexpr u32 transient = static_cast<u32>(sl::DLSSGStatus::eFailResolutionTooLow) |
                                      static_cast<u32>(sl::DLSSGStatus::eFailCommonConstantsInvalid);
            gen.failed = (status & ~transient) != 0;
            if (status != gen.last_transient_status || gen.failed) {
                LOG_WARNING(Render_Vulkan, "Ocarina DLSS-G runtime status {:#x}, retryable={}",
                            status, !gen.failed);
            }
            gen.last_transient_status = status;
            valid_scene = false;
        } else {
            gen.last_transient_status = 0;
            // The SDK counter spans all presents since its last query. Discard the first
            // sample after enabling or resuming, which can also include menu frames.
            if (std::exchange(gen.prime_present_counter, false)) {
                LOG_DEBUG(Render_Vulkan, "Ocarina DLSS-G started a new display measurement");
            } else {
                gen.window_displayed += result.numFramesActuallyPresented;
                if (++gen.window_real == 120) {
                    LOG_INFO(Render_Vulkan,
                             "Ocarina DLSS-G measured {:.2f}x: {} NVIDIA-reported displayed frames "
                             "over {} consecutive successful real presents (requested {}x)",
                             double(gen.window_displayed) / gen.window_real,
                             gen.window_displayed, gen.window_real, gen.generation_count + 1);
                    gen.window_real = gen.window_displayed = 0;
                }
            }
        }
    }
    if (!valid_scene) {
        SetGeneration(gen, false);
    }
    gen.last_presented_scene = valid_scene ? gen.pending_scene : std::nullopt;
    if (gen.last_presented_scene) {
        gen.last_presented_token = u32(*gen.token);
    }
    gen.pending_scene.reset();
    gen.token = nullptr;
    gen.rendering = gen.presenting = false;
    gen.tagged_width = gen.tagged_height = 0;
    if (present_succeeded) {
        StartSimulation(state);
    }
}

void DisableFrameGeneration() {
    auto& state = GetState();
    std::scoped_lock lock{state.generation_mutex};
    auto& gen = state.generation;
    if (gen.ready) {
        SetGeneration(gen, false);
        gen.token = nullptr;
        gen.rendering = gen.presenting = false;
        gen.tagged_width = gen.tagged_height = 0;
        gen.pending_scene.reset();
        gen.last_presented_scene.reset();
    }
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    DisableFrameGeneration();
    if (state.generation.generation_count) {
        state.api.free_resources(sl::kFeatureDLSS_G, GameViewport);
    }
    const sl::Result result = state.api.shutdown();
    state.generation = {};
    state.proxy_loader = nullptr;
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

const Api& GetApi() {
    return GetState().api;
}

} // namespace Vulkan::Streamline
