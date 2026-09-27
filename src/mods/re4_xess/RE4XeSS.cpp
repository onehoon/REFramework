#include "mods/re4_xess/RE4XeSS.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <sdk/GameIdentity.hpp>
#include <sdk/RETypeDB.hpp>
#include <sdk/SceneManager.hpp>
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

float halton(uint32_t sample_index, uint32_t base) {
    float result{};
    float fraction{ 1.0f };
    auto index = sample_index;

    while (index != 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }

    return result;
}

struct GameLoadSnapshot {
    bool pause{};
    uint64_t inhibit{};
};

struct LoadStateAccessors {
    sdk::RETypeDefinition* pause_type{};
    sdk::REMethodDefinition* pause_instance_getter{};
    sdk::REField* pause_field{};
    sdk::RETypeDefinition* situation_type{};
    sdk::REMethodDefinition* situation_instance_getter{};
    sdk::REField* inhibit_field{};

    void resolve() {
        if (pause_type == nullptr) {
            pause_type = sdk::find_type_definition("chainsaw.SceneLoadZoneManager");
        }
        if (pause_type != nullptr) {
            if (pause_instance_getter == nullptr) pause_instance_getter = pause_type->get_method("get_Instance");
            if (pause_field == nullptr) pause_field = pause_type->get_field("_Pause");
        }

        if (situation_type == nullptr) {
            situation_type = sdk::find_type_definition("chainsaw.GameSituationManager");
        }
        if (situation_type != nullptr) {
            if (situation_instance_getter == nullptr) situation_instance_getter = situation_type->get_method("get_Instance");
            if (inhibit_field == nullptr) inhibit_field = situation_type->get_field("InhibitBit");
        }
    }
};

bool is_integral_type(const sdk::RETypeDefinition* type) {
    if (type == nullptr) {
        return false;
    }

    const auto name = type->get_full_name();
    return name == "System.Byte" || name == "System.SByte" ||
        name == "System.UInt16" || name == "System.Int16" ||
        name == "System.UInt32" || name == "System.Int32" ||
        name == "System.UInt64" || name == "System.Int64";
}

std::optional<uint64_t> read_integral_field(sdk::REField* field, REManagedObject* instance) {
    if (field == nullptr || instance == nullptr) {
        return std::nullopt;
    }

    auto* field_type = field->get_type();
    if (field_type == nullptr) {
        return std::nullopt;
    }

    auto* storage_type = field_type->is_enum() ? field_type->get_underlying_type() : field_type;
    if (!is_integral_type(storage_type)) {
        return std::nullopt;
    }

    const auto width = storage_type->get_size();
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return std::nullopt;
    }

    const auto* address = field->get_data_raw(instance);
    if (address == nullptr) {
        return std::nullopt;
    }

    uint64_t value{};
    std::memcpy(&value, address, width);
    return value;
}

std::optional<bool> read_bool_field(sdk::REField* field, REManagedObject* instance) {
    if (field == nullptr || instance == nullptr) {
        return std::nullopt;
    }

    auto* type = field->get_type();
    if (type == nullptr || type->get_full_name() != "System.Boolean" || type->get_size() != sizeof(uint8_t)) {
        return std::nullopt;
    }

    const auto* address = field->get_data_raw(instance);
    if (address == nullptr) {
        return std::nullopt;
    }

    uint8_t value{};
    std::memcpy(&value, address, sizeof(value));
    if (value > 1) {
        return std::nullopt;
    }

    return value != 0;
}

std::optional<GameLoadSnapshot> read_game_load_snapshot() {
    static LoadStateAccessors accessors{};
    accessors.resolve();

    if (accessors.pause_instance_getter == nullptr || accessors.pause_field == nullptr ||
        accessors.situation_instance_getter == nullptr || accessors.inhibit_field == nullptr) {
        return std::nullopt;
    }

    auto* pause_manager = accessors.pause_instance_getter->call_safe<REManagedObject*>(sdk::get_thread_context());
    auto* situation_manager = accessors.situation_instance_getter->call_safe<REManagedObject*>(sdk::get_thread_context());
    if (pause_manager == nullptr || situation_manager == nullptr) {
        return std::nullopt;
    }

    const auto pause = read_bool_field(accessors.pause_field, pause_manager);
    const auto inhibit = read_integral_field(accessors.inhibit_field, situation_manager);
    if (!pause || !inhibit) {
        return std::nullopt;
    }

    return GameLoadSnapshot{ *pause, *inhibit };
}

std::array<sdk::renderer::SceneInfo*, 6> scene_infos(sdk::renderer::layer::Scene* layer) {
    return {
        layer->get_scene_info(),
        layer->get_depth_distortion_scene_info(),
        layer->get_filter_scene_info(),
        layer->get_jitter_disable_scene_info(),
        layer->get_jitter_disable_post_scene_info(),
        layer->get_z_prepass_scene_info(),
    };
}

constexpr std::array<std::string_view, 6> SCENE_INFO_NAMES{
    "main", "depthDistortion", "filter", "jitterDisable", "jitterDisablePost", "zPrepass",
};

bool is_valid_texture_extent(ID3D12Resource* resource, uint32_t width, uint32_t height) {
    if (resource == nullptr) {
        return false;
    }

    const auto description = resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height &&
        description.SampleDesc.Count == 1;
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
    clear_frame_state();

    if (!sdk::GameIdentity::get().is_re4()) {
        if (m_runtime.state() != RE4XeSSRuntime::State::Unloaded) {
            m_runtime.shutdown();
        }
        m_device_identity = nullptr;
        m_bootstrap_pending = false;
        m_transition_pending = false;
        reset_temporal_state("non-re4", true);
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
            reset_temporal_state("d3d12-device-change", false);
        }
    }

    if (m_bootstrap_pending) {
        try_bootstrap();
    }

    if (m_requested_mode == UpscalingMode::Off) {
        reset_temporal_state("mode-off", true);
        return;
    }

    update_load_state();
    update_temporal_configuration();
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

    if (m_temporal_ready) {
        ImGui::TextUnformatted("Temporal inputs active - PR2 validation only; XeSS execute not active");
        ImGui::Text("Display: %ux%u  Input: %ux%u  Generation: %llu",
            m_display_resolution.x,
            m_display_resolution.y,
            m_input_resolution.optimal.x,
            m_input_resolution.optimal.y,
            static_cast<unsigned long long>(m_temporal_generation));
        return;
    }

    if (!m_temporal_failure_reason.empty()) {
        ImGui::TextWrapped("Temporal setup unavailable: %s", m_temporal_failure_reason.c_str());
        return;
    }

    switch (m_runtime.state()) {
    case RE4XeSSRuntime::State::ContextReady:
        ImGui::TextUnformatted("Context ready - waiting for display/input resolution");
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
    reset_temporal_state("d3d12-device-reset", false);

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
    reset_temporal_state(mode == UpscalingMode::Off ? "mode-off" : "mode-change", mode == UpscalingMode::Off);

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
    reset_temporal_state(m_requested_mode == UpscalingMode::Off ? "mode-off" : "runtime-context-recreate",
        m_requested_mode == UpscalingMode::Off);

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

bool RE4XeSS::get_display_resolution(xess_2d_t& resolution) const {
    resolution = {};

    if (g_framework == nullptr || g_framework->get_renderer_type() != REFramework::RendererType::D3D12) {
        return false;
    }

    const auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr) {
        return false;
    }

    auto* swapchain = hook->get_swap_chain();
    if (swapchain == nullptr) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    if (FAILED(swapchain->GetDesc1(&description)) || description.Width == 0 || description.Height == 0) {
        return false;
    }

    resolution = { description.Width, description.Height };
    return true;
}

bool RE4XeSS::is_temporal_active() const {
    return sdk::GameIdentity::get().is_re4() &&
        m_requested_mode != UpscalingMode::Off &&
        m_runtime.state() == RE4XeSSRuntime::State::ContextReady &&
        m_temporal_ready;
}

void RE4XeSS::invalidate_history(std::string_view reason, bool reset_jitter) {
    const bool was_valid = !m_history_invalid || !m_first_valid_frame_reset_pending;
    m_history_invalid = true;
    m_first_valid_frame_reset_pending = true;
    m_scene_history = {};
    m_last_processed_scene_frame.reset();
    m_next_jitter_sample = 0;

    if (reason != m_last_reset_reason && REFrameworkConfig::get()->is_debug_log_enabled()) {
        spdlog::info("[RE4XeSS][Reset] reason={}", reason);
    }
    m_last_reset_reason = reason;

    if (reset_jitter) {
        m_next_jitter_sample = 0;
    }

    if (was_valid) {
        m_latest_frame_snapshot.reset();
    }
}

void RE4XeSS::clear_frame_state() {
    m_cached_scene = nullptr;
    m_cached_scene_frame.reset();
    m_cached_jitter_x = 0.0f;
    m_cached_jitter_y = 0.0f;
    m_cached_vertical_fov = 0.0f;
    m_camera_near = 0.0f;
    m_camera_far = 0.0f;
    m_camera_frame.reset();
    m_camera_metadata_valid = false;
}

void RE4XeSS::clear_resource_identities() {
    m_resource_identity_valid = false;
    m_last_color_identity = 0;
    m_last_depth_identity = 0;
    m_last_velocity_identity = 0;
}

void RE4XeSS::reset_temporal_state(std::string_view reason, bool reset_load_state) {
    m_temporal_ready = false;
    m_temporal_signature_valid = false;
    m_input_resolution_valid = false;
    m_temporal_failure_reason.clear();
    m_display_resolution = {};
    m_input_resolution = {};
    m_jitter_phase_count = 8;
    clear_resource_identities();
    m_latest_frame_snapshot.reset();
    m_last_scene_callback_frame.reset();
    clear_frame_state();
    invalidate_history(reason);

    if (!reset_load_state) {
        return;
    }

    m_pause_previous_valid = false;
    m_pause_previous = false;
    m_load_transition_active = false;
    m_inhibit_departure_pending = false;
    m_remembered_normal_inhibit_valid = false;
    m_remembered_normal_inhibit = 0;
    m_departure_inhibit = 0;
    m_startup_mid_load = false;
    m_post_pause_rebaseline_candidate_valid = false;
    m_post_pause_rebaseline_candidate = 0;
    m_post_pause_rebaseline_stable_count = 0;
    m_load_observation_valid = false;
}

void RE4XeSS::update_temporal_configuration() {
    const auto previous_ready = m_temporal_ready;
    auto set_unavailable = [&](std::string reason, std::string_view reset_reason) {
        if (m_temporal_ready) {
            invalidate_history(reset_reason);
            clear_resource_identities();
            m_temporal_signature_valid = false;
            m_input_resolution_valid = false;
        }
        m_temporal_ready = false;
        if (m_temporal_failure_reason != reason && REFrameworkConfig::get()->is_debug_log_enabled()) {
            spdlog::warn("[RE4XeSS][Temporal] {}", reason);
        }
        m_temporal_failure_reason = std::move(reason);
    };

    if (!sdk::GameIdentity::get().is_re4() || m_requested_mode == UpscalingMode::Off) {
        set_unavailable("Off - native RE4 rendering", "mode-off");
        return;
    }

    if (m_runtime.state() != RE4XeSSRuntime::State::ContextReady) {
        const auto reason = m_runtime.state() == RE4XeSSRuntime::State::Faulted
            ? m_runtime.failure_reason()
            : std::string{ "Waiting for the RE4 XeSS runtime context" };
        set_unavailable(reason, "runtime-context-unavailable");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    const auto quality = mode_to_quality_setting(m_requested_mode);
    if (!quality) {
        set_unavailable("The requested RE4 upscaling mode has no XeSS quality mapping", "invalid-quality-mode");
        return;
    }

    xess_2d_t display{};
    if (!get_display_resolution(display)) {
        set_unavailable("Waiting for a valid nonzero D3D12 swapchain display extent", "display-extent-unavailable");
        m_temporal_signature_valid = false;
        m_input_resolution_valid = false;
        return;
    }

    const bool configuration_changed =
        !m_temporal_signature_valid ||
        m_temporal_mode != m_requested_mode ||
        m_temporal_quality != *quality ||
        m_display_resolution.x != display.x ||
        m_display_resolution.y != display.y;

    if (configuration_changed) {
        if (m_temporal_signature_valid || m_temporal_generation == 0) {
            invalidate_history("temporal-configuration-change");
        }
        ++m_temporal_generation;
        m_temporal_mode = m_requested_mode;
        m_temporal_quality = *quality;
        m_display_resolution = display;
        m_temporal_signature_valid = true;
        m_temporal_ready = false;
        m_input_resolution_valid = false;
        clear_resource_identities();
    }

    std::string query_error;
    const auto query = m_runtime.query_optimal_input_resolution(display, *quality, query_error);
    if (!query) {
        set_unavailable(query_error, "input-resolution-query-failed");
        return;
    }

    const auto render = query->optimal;
    if (render.x > display.x || render.y > display.y) {
        set_unavailable(
            "XeSS frontend optimal input extent exceeds the display extent",
            "input-extent-exceeds-display");
        return;
    }

    const uint64_t render_display_cross = static_cast<uint64_t>(render.x) * display.y;
    const uint64_t display_render_cross = static_cast<uint64_t>(render.y) * display.x;
    const uint64_t cross_error = render_display_cross > display_render_cross
        ? render_display_cross - display_render_cross
        : display_render_cross - render_display_cross;
    const auto aspect_denominator = static_cast<double>(render.y) * display.x;
    const auto relative_aspect_error = aspect_denominator > 0.0
        ? static_cast<double>(cross_error) / aspect_denominator
        : std::numeric_limits<double>::infinity();

    if (!std::isfinite(relative_aspect_error) || relative_aspect_error > 0.01) {
        set_unavailable(
            "XeSS frontend aspect mismatch: display=" + std::to_string(display.x) + "x" + std::to_string(display.y) +
                " input=" + std::to_string(render.x) + "x" + std::to_string(render.y) +
                " relativeError=" + std::to_string(relative_aspect_error),
            "input-aspect-mismatch");
        return;
    }

    const bool query_changed = !m_input_resolution_valid ||
        m_input_resolution.optimal.x != query->optimal.x ||
        m_input_resolution.optimal.y != query->optimal.y ||
        m_input_resolution.minimum.x != query->minimum.x ||
        m_input_resolution.minimum.y != query->minimum.y ||
        m_input_resolution.maximum.x != query->maximum.x ||
        m_input_resolution.maximum.y != query->maximum.y;

    if (query_changed) {
        if (!configuration_changed) {
            ++m_temporal_generation;
            invalidate_history("frontend-input-resolution-change");
            clear_resource_identities();
        }
        m_input_resolution = *query;
        m_input_resolution_valid = true;
    } else if (!previous_ready) {
        ++m_temporal_generation;
        invalidate_history("temporal-setup-recovered");
    }

    const auto scale_x = static_cast<double>(display.x) / render.x;
    const auto scale_y = static_cast<double>(display.y) / render.y;
    const auto scale = std::max(scale_x, scale_y);
    const auto requested_phase_count = std::max(8.0, std::ceil(8.0 * scale * scale));
    m_jitter_phase_count = static_cast<uint32_t>(std::min(
        requested_phase_count,
        static_cast<double>(std::numeric_limits<uint32_t>::max())));
    m_temporal_ready = true;
    m_temporal_failure_reason.clear();

    if ((!previous_ready || configuration_changed || query_changed) && REFrameworkConfig::get()->is_debug_log_enabled()) {
        spdlog::info(
            "[RE4XeSS][Temporal] mode={} display={}x{} input={}x{} min={}x{} max={}x{} phases={} generation={}",
            mode_to_display_label(m_requested_mode),
            display.x,
            display.y,
            query->optimal.x,
            query->optimal.y,
            query->minimum.x,
            query->minimum.y,
            query->maximum.x,
            query->maximum.y,
            m_jitter_phase_count,
            static_cast<unsigned long long>(m_temporal_generation));
    }
}

void RE4XeSS::update_load_state() {
    const auto snapshot = read_game_load_snapshot();
    if (!snapshot) {
        m_load_observation_valid = false;
        invalidate_history("load-state-observation-unavailable");
        return;
    }

    const auto log_load_event = [](std::string_view message) {
        if (REFrameworkConfig::get()->is_debug_log_enabled()) {
            spdlog::info("[RE4XeSS][Reset] {}", message);
        }
    };

    m_load_observation_valid = true;

    if (!m_pause_previous_valid) {
        m_pause_previous_valid = true;
        m_pause_previous = snapshot->pause;
        if (snapshot->pause) {
            m_startup_mid_load = true;
            m_load_transition_active = true;
            m_remembered_normal_inhibit_valid = false;
            m_post_pause_rebaseline_candidate_valid = false;
            m_post_pause_rebaseline_stable_count = 0;
            invalidate_history("startup-observed-mid-load");
            log_load_event("load pause observed at startup; no pre-load baseline assumed");
        } else if (!m_remembered_normal_inhibit_valid) {
            m_remembered_normal_inhibit = snapshot->inhibit;
            m_remembered_normal_inhibit_valid = true;
        }
        return;
    }

    if (snapshot->pause) {
        if (!m_pause_previous) {
            if (m_remembered_normal_inhibit_valid && snapshot->inhibit != m_remembered_normal_inhibit) {
                m_departure_inhibit = snapshot->inhibit;
            }

            m_load_transition_active = true;
            m_inhibit_departure_pending = false;
            if (!m_remembered_normal_inhibit_valid) {
                m_startup_mid_load = true;
            }
            m_post_pause_rebaseline_candidate_valid = false;
            m_post_pause_rebaseline_stable_count = 0;
            invalidate_history("load-pause-entered");
            log_load_event("load pause entered; frozen normal InhibitBit preserved");
        }
        m_pause_previous = true;
        return;
    }

    const bool pause_just_released = m_pause_previous;
    m_pause_previous = false;

    if (m_startup_mid_load) {
        if (pause_just_released) {
            m_post_pause_rebaseline_candidate = snapshot->inhibit;
            m_post_pause_rebaseline_candidate_valid = true;
            m_post_pause_rebaseline_stable_count = 1;
            return;
        }

        if (!m_post_pause_rebaseline_candidate_valid) {
            m_post_pause_rebaseline_candidate = snapshot->inhibit;
            m_post_pause_rebaseline_candidate_valid = true;
            m_post_pause_rebaseline_stable_count = 1;
            return;
        }

        if (snapshot->inhibit != m_post_pause_rebaseline_candidate) {
            m_post_pause_rebaseline_candidate = snapshot->inhibit;
            m_post_pause_rebaseline_stable_count = 1;
            return;
        }

        if (m_post_pause_rebaseline_stable_count < 3) {
            ++m_post_pause_rebaseline_stable_count;
        }
        if (m_post_pause_rebaseline_stable_count >= 3) {
            m_remembered_normal_inhibit = m_post_pause_rebaseline_candidate;
            m_remembered_normal_inhibit_valid = true;
            m_startup_mid_load = false;
            m_load_transition_active = false;
            m_inhibit_departure_pending = false;
            m_first_valid_frame_reset_pending = true;
            invalidate_history("startup-load-rebaseline-complete");
            log_load_event("startup-mid-load fallback rebaseline stable for three observations");
        }
        return;
    }

    if (m_load_transition_active) {
        if (!m_remembered_normal_inhibit_valid) {
            return;
        }

        if (snapshot->inhibit != m_remembered_normal_inhibit) {
            m_departure_inhibit = snapshot->inhibit;
            return;
        }

        m_load_transition_active = false;
        m_inhibit_departure_pending = false;
        m_post_pause_rebaseline_candidate_valid = false;
        m_post_pause_rebaseline_stable_count = 0;
        m_first_valid_frame_reset_pending = true;
        invalidate_history("load-recovery-complete");
        log_load_event("load recovery completed after InhibitBit returned to frozen baseline");
        return;
    }

    if (!m_remembered_normal_inhibit_valid) {
        m_remembered_normal_inhibit = snapshot->inhibit;
        m_remembered_normal_inhibit_valid = true;
        return;
    }

    if (snapshot->inhibit != m_remembered_normal_inhibit) {
        if (!m_inhibit_departure_pending) {
            m_inhibit_departure_pending = true;
            m_departure_inhibit = snapshot->inhibit;
            invalidate_history("pre-pause-inhibit-departure");
            log_load_event("InhibitBit departed before Pause; preserving the previous normal baseline");
        }
        return;
    }

    if (m_inhibit_departure_pending) {
        m_inhibit_departure_pending = false;
        m_departure_inhibit = 0;
        invalidate_history("pre-pause-inhibit-return");
        log_load_event("InhibitBit returned before Pause; suspicion cleared with history reset retained");
    }
}

void RE4XeSS::on_view_get_size(REManagedObject* scene_view, float* result) {
    (void)scene_view;
    if (!is_temporal_active() || result == nullptr ||
        m_input_resolution.optimal.x == 0 || m_input_resolution.optimal.y == 0) {
        return;
    }

    result[0] = static_cast<float>(m_input_resolution.optimal.x);
    result[1] = static_cast<float>(m_input_resolution.optimal.y);
}

void RE4XeSS::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (!is_temporal_active() || camera == nullptr || result == nullptr) {
        return;
    }

    const auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || camera != reinterpret_cast<REManagedObject*>(const_cast<RECamera*>(primary_camera))) {
        return;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame) {
        return;
    }

    static sdk::RETypeDefinition* camera_type{};
    static sdk::REMethodDefinition* get_near_clip_plane{};
    static sdk::REMethodDefinition* get_far_clip_plane{};
    if (camera_type == nullptr) {
        camera_type = sdk::find_type_definition("via.Camera");
    }
    if (camera_type != nullptr) {
        if (get_near_clip_plane == nullptr) get_near_clip_plane = camera_type->get_method("get_NearClipPlane");
        if (get_far_clip_plane == nullptr) get_far_clip_plane = camera_type->get_method("get_FarClipPlane");
    }

    m_camera_frame = *frame;
    m_camera_metadata_valid = false;
    if (get_near_clip_plane == nullptr || get_far_clip_plane == nullptr) {
        return;
    }

    const auto near_plane = get_near_clip_plane->call_safe<float>(sdk::get_thread_context(), camera);
    const auto far_plane = get_far_clip_plane->call_safe<float>(sdk::get_thread_context(), camera);
    if (!std::isfinite(near_plane) || !std::isfinite(far_plane) || near_plane <= 0.0f || far_plane <= near_plane) {
        return;
    }

    m_camera_near = near_plane;
    m_camera_far = far_plane;
    m_camera_metadata_valid = true;
}

void RE4XeSS::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;
    if (!is_temporal_active() || layer == nullptr || !layer->is_fully_rendered()) {
        return;
    }

    const auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || layer->get_camera() != primary_camera) {
        return;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame) {
        invalidate_history("renderer-frame-id-unavailable");
        return;
    }

    const auto frame_id = static_cast<uint64_t>(*frame);
    if (m_last_scene_callback_frame && *m_last_scene_callback_frame == frame_id) {
        return;
    }
    m_last_scene_callback_frame = frame_id;

    const bool frame_gap = m_last_processed_scene_frame &&
        frame_id != (*m_last_processed_scene_frame + 1);
    if (frame_gap) {
        invalidate_history("primary-scene-frame-gap");
    }
    m_last_processed_scene_frame = frame_id;

    if (m_load_transition_active || m_startup_mid_load || !m_load_observation_valid) {
        invalidate_history("load-history-invalid");
        return;
    }

    if (!m_camera_frame || *m_camera_frame != frame_id || !m_camera_metadata_valid) {
        invalidate_history("same-frame-camera-metadata-unavailable");
        return;
    }

    auto infos = scene_infos(layer);
    if (infos[0] == nullptr) {
        invalidate_history("primary-scene-info-unavailable");
        return;
    }

    const auto projection_y = infos[0]->projection_matrix[1][1];
    if (!std::isfinite(projection_y) || std::abs(projection_y) < 0.000001f) {
        invalidate_history("primary-scene-fov-invalid");
        return;
    }
    const auto vertical_fov = 2.0f * std::atan(1.0f / projection_y);
    if (!std::isfinite(vertical_fov) || vertical_fov <= 0.0f) {
        invalidate_history("primary-scene-fov-invalid");
        return;
    }

    const bool variant_frame_gap = [&] {
        for (size_t i = 0; i < infos.size(); ++i) {
            if (infos[i] != nullptr && m_scene_history[i].valid &&
                frame_id != m_scene_history[i].frame_id + 1) {
                return true;
            }
        }
        return false;
    }();
    if (variant_frame_gap) {
        invalidate_history("scene-info-history-gap");
    }

    const auto sample_index = m_next_jitter_sample + 1;
    const auto jitter_x = halton(sample_index, 2) - 0.5f;
    const auto jitter_y = halton(sample_index, 3) - 0.5f;
    const auto matrix_jitter_x = 2.0f * jitter_x / static_cast<float>(m_input_resolution.optimal.x);
    const auto matrix_jitter_y = -2.0f * jitter_y / static_cast<float>(m_input_resolution.optimal.y);

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            continue;
        }

        const auto current_projection = info->projection_matrix;
        const auto current_view = info->view_matrix;
        auto& history = m_scene_history[i];
        const bool history_valid = history.valid && frame_id == history.frame_id + 1;
        auto previous_projection = history_valid ? history.unjittered_projection : current_projection;
        const auto previous_view = history_valid ? history.view : current_view;

        previous_projection[2][0] += matrix_jitter_x;
        previous_projection[2][1] += matrix_jitter_y;
        info->old_view_projection_matrix = previous_projection * previous_view;

        history.unjittered_projection = current_projection;
        history.view = current_view;
        history.frame_id = frame_id;
        history.valid = true;

        info->projection_matrix[2][0] += matrix_jitter_x;
        info->projection_matrix[2][1] += matrix_jitter_y;
        info->inverse_projection_matrix = glm::inverse(info->projection_matrix);
        info->view_projection_matrix = info->projection_matrix * info->view_matrix;
        info->inverse_view_projection_matrix = glm::inverse(info->view_projection_matrix);
    }

    m_next_jitter_sample = (m_next_jitter_sample + 1) % m_jitter_phase_count;
    m_cached_scene = layer;
    m_cached_scene_frame = frame_id;
    m_cached_jitter_x = jitter_x;
    m_cached_jitter_y = jitter_y;
    m_cached_vertical_fov = vertical_fov;
}

bool RE4XeSS::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    (void)render_context;
    if (!is_temporal_active()) {
        clear_frame_state();
        return true;
    }
    if (layer == nullptr) {
        invalidate_history("pre-overlay-layer-unavailable");
        clear_frame_state();
        return true;
    }

    const auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame || !m_cached_scene || !m_cached_scene_frame ||
        *m_cached_scene_frame != static_cast<uint64_t>(*frame)) {
        invalidate_history("pre-overlay-primary-scene-frame-missing");
        clear_frame_state();
        return true;
    }

    if (m_load_transition_active || m_startup_mid_load || !m_load_observation_valid ||
        !m_camera_metadata_valid || !m_camera_frame || *m_camera_frame != *m_cached_scene_frame) {
        invalidate_history("pre-overlay-temporal-gate-invalid");
        clear_frame_state();
        return true;
    }

    auto* color_state = layer->get_main_target_state().get();
    auto* color = color_state != nullptr ? color_state->get_native_resource_d3d12() : nullptr;
    auto* scene = m_cached_scene;
    auto* depth = scene->get_depth_stencil_d3d12();
    auto* velocity = scene->get_motion_vectors_d3d12();
    if (color == nullptr || depth == nullptr || velocity == nullptr ||
        color != scene->get_post_main_target_d3d12() || color != scene->get_hdr_target_d3d12()) {
        invalidate_history("temporal-resource-identity-invariant-failed");
        clear_frame_state();
        return true;
    }

    const auto render_width = m_input_resolution.optimal.x;
    const auto render_height = m_input_resolution.optimal.y;
    if (!is_valid_texture_extent(color, render_width, render_height) ||
        !is_valid_texture_extent(depth, render_width, render_height) ||
        !is_valid_texture_extent(velocity, render_width, render_height)) {
        invalidate_history("temporal-resource-extent-invariant-failed");
        clear_frame_state();
        return true;
    }

    const auto color_identity = reinterpret_cast<uintptr_t>(color);
    const auto depth_identity = reinterpret_cast<uintptr_t>(depth);
    const auto velocity_identity = reinterpret_cast<uintptr_t>(velocity);
    const bool resource_identity_changed = m_resource_identity_valid &&
        (color_identity != m_last_color_identity ||
            depth_identity != m_last_depth_identity ||
            velocity_identity != m_last_velocity_identity);
    if (resource_identity_changed) {
        invalidate_history("temporal-resource-identity-change");
    }

    RE4XeSSFrame packet{};
    packet.color = color;
    packet.depth = depth;
    packet.velocity = velocity;
    packet.render_width = render_width;
    packet.render_height = render_height;
    packet.display_width = m_display_resolution.x;
    packet.display_height = m_display_resolution.y;
    packet.jitter_x_pixels = m_cached_jitter_x;
    packet.jitter_y_pixels = m_cached_jitter_y;
    packet.motion_scale_x = static_cast<float>(render_width) / 2.0f;
    packet.motion_scale_y = -static_cast<float>(render_height) / 2.0f;
    packet.near_plane = m_camera_near;
    packet.far_plane = m_camera_far;
    packet.vertical_fov = m_cached_vertical_fov;
    packet.reset_history = m_first_valid_frame_reset_pending || m_history_invalid;
    packet.frame_id = *m_cached_scene_frame;

    m_last_color_identity = color_identity;
    m_last_depth_identity = depth_identity;
    m_last_velocity_identity = velocity_identity;
    m_resource_identity_valid = true;
    m_latest_frame_snapshot = FrameSnapshot{
        packet.frame_id,
        color_identity,
        depth_identity,
        velocity_identity,
        packet.render_width,
        packet.render_height,
        packet.display_width,
        packet.display_height,
        packet.jitter_x_pixels,
        packet.jitter_y_pixels,
        packet.motion_scale_x,
        packet.motion_scale_y,
        packet.near_plane,
        packet.far_plane,
        packet.vertical_fov,
        packet.reset_history,
    };

    if (packet.reset_history && REFrameworkConfig::get()->is_debug_log_enabled()) {
        spdlog::info("[RE4XeSS][Frame] first valid packet after generation change: frame={} input={}x{} display={}x{}",
            static_cast<unsigned long long>(packet.frame_id),
            packet.render_width,
            packet.render_height,
            packet.display_width,
            packet.display_height);
    }

    m_first_valid_frame_reset_pending = false;
    m_history_invalid = false;
    m_last_reset_reason.clear();
    clear_frame_state();
    return true;
}
