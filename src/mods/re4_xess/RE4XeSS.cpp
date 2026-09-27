#include "mods/re4_xess/RE4XeSS.hpp"

#include <array>
#include <filesystem>
#include <optional>

#include <sdk/GameIdentity.hpp>
#include <spdlog/spdlog.h>
#include <utility/Module.hpp>

#include "mods/REFrameworkConfig.hpp"

namespace {

using UpscalingMode = RE4XeSS::UpscalingMode;

constexpr std::string_view UPSCALING_MODE_CONFIG_KEY{ "RE4XeSS_UpscalingMode" };

constexpr std::array<const char*, 8> UPSCALING_MODE_LABELS{
    "Off",
    "Native AA",
    "Ultra Quality Plus",
    "Ultra Quality",
    "Quality",
    "Balanced",
    "Performance",
    "Ultra Performance",
};

std::optional<UpscalingMode> mode_from_config_token(std::string_view token) {
    if (token == "off") return UpscalingMode::Off;
    if (token == "native_aa") return UpscalingMode::NativeAA;
    if (token == "ultra_quality_plus") return UpscalingMode::UltraQualityPlus;
    if (token == "ultra_quality") return UpscalingMode::UltraQuality;
    if (token == "quality") return UpscalingMode::Quality;
    if (token == "balanced") return UpscalingMode::Balanced;
    if (token == "performance") return UpscalingMode::Performance;
    if (token == "ultra_performance") return UpscalingMode::UltraPerformance;
    return std::nullopt;
}

std::string_view mode_to_config_token(UpscalingMode mode) {
    switch (mode) {
    case UpscalingMode::Off: return "off";
    case UpscalingMode::NativeAA: return "native_aa";
    case UpscalingMode::UltraQualityPlus: return "ultra_quality_plus";
    case UpscalingMode::UltraQuality: return "ultra_quality";
    case UpscalingMode::Quality: return "quality";
    case UpscalingMode::Balanced: return "balanced";
    case UpscalingMode::Performance: return "performance";
    case UpscalingMode::UltraPerformance: return "ultra_performance";
    }
    return "off";
}

std::string_view mode_to_display_label(UpscalingMode mode) {
    const auto index = static_cast<size_t>(mode);
    return index < UPSCALING_MODE_LABELS.size() ? UPSCALING_MODE_LABELS[index] : UPSCALING_MODE_LABELS[0];
}

std::optional<xess_quality_settings_t> mode_to_quality_setting(UpscalingMode mode) {
    switch (mode) {
    case UpscalingMode::Off: return std::nullopt;
    case UpscalingMode::NativeAA: return XESS_QUALITY_SETTING_AA;
    case UpscalingMode::UltraQualityPlus: return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
    case UpscalingMode::UltraQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case UpscalingMode::Quality: return XESS_QUALITY_SETTING_QUALITY;
    case UpscalingMode::Balanced: return XESS_QUALITY_SETTING_BALANCED;
    case UpscalingMode::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
    case UpscalingMode::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    }
    return std::nullopt;
}

std::string path_for_log(const std::filesystem::path& path) {
    return path.empty() ? std::string{ "<unavailable>" } : utility::narrow(path.c_str());
}

}

RE4XeSS::~RE4XeSS() {
    m_runtime.shutdown();
}

std::optional<std::string> RE4XeSS::on_initialize() {
    return std::nullopt;
}

std::optional<std::string> RE4XeSS::on_initialize_d3d_thread() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return std::nullopt;
    }

    if (m_transition_pending) {
        apply_pending_transition();
    } else if (m_requested_mode != UpscalingMode::Off) {
        m_bootstrap_pending = true;
        try_bootstrap();
    }

    return std::nullopt;
}

void RE4XeSS::on_frame() {
    if (!sdk::GameIdentity::get().is_re4()) {
        if (m_runtime.state() != RE4XeSSRuntime::State::Unloaded) {
            m_runtime.shutdown();
        }
        m_device_identity = nullptr;
        m_bootstrap_pending = false;
        m_transition_pending = false;
        return;
    }

    if (m_transition_pending) {
        apply_pending_transition();
    }

    if (m_runtime.state() == RE4XeSSRuntime::State::ContextReady) {
        ID3D12Device* current_device{};
        if (g_framework != nullptr && g_framework->get_renderer_type() == REFramework::RendererType::D3D12) {
            const auto& hook = g_framework->get_d3d12_hook();
            if (hook != nullptr) {
                current_device = hook->get_device();
            }
        }

        if (current_device != m_device_identity) {
            m_runtime.shutdown();
            m_device_identity = nullptr;
            m_bootstrap_pending = m_requested_mode != UpscalingMode::Off;
        }
    }

    if (m_bootstrap_pending) {
        try_bootstrap();
    }
}

void RE4XeSS::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    ImGui::TextUnformatted("RE4 XeSS");

    auto selected_mode = static_cast<int32_t>(m_requested_mode);
    if (ImGui::Combo("Upscaling Mode", &selected_mode, UPSCALING_MODE_LABELS.data(), static_cast<int32_t>(UPSCALING_MODE_LABELS.size()))) {
        request_mode(static_cast<UpscalingMode>(selected_mode));
        if (g_framework != nullptr) {
            g_framework->request_save_config();
        }
    }

    if (m_requested_mode == UpscalingMode::Off) {
        ImGui::TextUnformatted("Off - native RE4 rendering");
        return;
    }

    switch (m_runtime.state()) {
    case RE4XeSSRuntime::State::ContextReady:
        ImGui::TextUnformatted("Context ready - PR1 bootstrap only; SR execute not active");
        break;
    case RE4XeSSRuntime::State::Faulted:
        ImGui::TextWrapped("XeSS runtime unavailable: %s", m_runtime.failure_reason().c_str());
        break;
    case RE4XeSSRuntime::State::ModuleReady:
        ImGui::TextUnformatted("XeSS runtime loaded; creating context");
        break;
    case RE4XeSSRuntime::State::Unloaded:
        ImGui::TextUnformatted(m_bootstrap_pending
            ? "Waiting for the RE4 D3D12 device"
            : "XeSS bootstrap is not active");
        break;
    }
}

void RE4XeSS::on_device_reset() {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    const auto had_runtime = m_runtime.state() != RE4XeSSRuntime::State::Unloaded;
    m_runtime.shutdown();
    m_device_identity = nullptr;
    m_bootstrap_pending = m_requested_mode != UpscalingMode::Off;

    if (had_runtime) {
        spdlog::info("[RE4XeSS][Runtime] Device reset shut down the XeSS context");
    }
}

void RE4XeSS::on_config_load(const utility::Config& cfg) {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    const auto persisted_mode = cfg.get<std::string>(std::string{ UPSCALING_MODE_CONFIG_KEY });
    if (!persisted_mode) {
        return;
    }

    const auto parsed_mode = mode_from_config_token(*persisted_mode);
    const auto next_mode = parsed_mode.value_or(UpscalingMode::Off);
    if (!parsed_mode && m_last_invalid_config_token != *persisted_mode) {
        spdlog::warn("[RE4XeSS][Config] Unknown Upscaling Mode token '{}'; falling back to Off", *persisted_mode);
        m_last_invalid_config_token = *persisted_mode;
    } else if (parsed_mode) {
        m_last_invalid_config_token.clear();
    }

    request_mode(next_mode);
}

void RE4XeSS::on_config_save(utility::Config& cfg) {
    if (!sdk::GameIdentity::get().is_re4()) {
        return;
    }

    cfg.set<std::string>(
        std::string{ UPSCALING_MODE_CONFIG_KEY },
        std::string{ mode_to_config_token(m_requested_mode) });
}

void RE4XeSS::request_mode(UpscalingMode mode) {
    const auto old_mode = m_requested_mode;
    if (mode == old_mode) {
        return;
    }

    m_requested_mode = mode;
    m_transition_pending = true;
    m_bootstrap_pending = mode != UpscalingMode::Off;

    spdlog::info("[RE4XeSS][Config] mode changed: {} -> {}",
        mode_to_display_label(old_mode), mode_to_display_label(mode));

    if (REFrameworkConfig::get()->is_debug_log_enabled()) {
        const auto quality = mode_to_quality_setting(mode);
        if (quality) {
            spdlog::info("[RE4XeSS][Config] selected public XeSS quality enum={}", static_cast<int32_t>(*quality));
        }
    }
}

void RE4XeSS::apply_pending_transition() {
    m_transition_pending = false;
    m_runtime.shutdown();
    m_device_identity = nullptr;
    m_bootstrap_pending = m_requested_mode != UpscalingMode::Off;

    if (m_bootstrap_pending) {
        try_bootstrap();
    }
}

void RE4XeSS::try_bootstrap() {
    if (!m_bootstrap_pending || m_requested_mode == UpscalingMode::Off || !sdk::GameIdentity::get().is_re4()) {
        return;
    }

    if (g_framework == nullptr || g_framework->get_renderer_type() != REFramework::RendererType::D3D12) {
        return;
    }

    const auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return;
    }

    auto* device = hook->get_device();
    if (device == nullptr) {
        return;
    }

    // Consume the retry before loading; a failed runtime stays Faulted until a
    // mode transition or device reset explicitly requests another bootstrap.
    m_bootstrap_pending = false;

    std::filesystem::path reframework_directory;
    const auto reframework_module = REFramework::get_reframework_module();
    if (reframework_module != nullptr) {
        if (const auto module_path = utility::get_module_pathw(reframework_module)) {
            reframework_directory = std::filesystem::path{ *module_path }.parent_path();
        }
    }

    const auto initialized = m_runtime.initialize(device, reframework_directory);
    const auto debug_log_enabled = REFrameworkConfig::get()->is_debug_log_enabled();

    if (debug_log_enabled) {
        const auto& candidates = m_runtime.candidates();
        spdlog::info("[RE4XeSS][Runtime] REFramework directory='{}'; candidate 1='{}'; candidate 2='{}'; selected='{}'; load result={}",
            path_for_log(reframework_directory),
            path_for_log(candidates[0]),
            path_for_log(candidates[1]),
            path_for_log(m_runtime.selected_path()),
            initialized ? "success" : "failure");

        if (const auto& version = m_runtime.runtime_version()) {
            spdlog::info("[RE4XeSS][Runtime] XeSS version={}.{}.{}; D3D12 device=0x{:x}",
                version->major,
                version->minor,
                version->patch,
                reinterpret_cast<uintptr_t>(device));
        }
    }

    if (!initialized) {
        m_device_identity = nullptr;
        spdlog::error("[RE4XeSS][Failure] {}", m_runtime.failure_reason());
        return;
    }

    m_device_identity = device;
    spdlog::info("[RE4XeSS][Runtime] XeSS context ready; PR1 bootstrap only, no SR execute");
}
