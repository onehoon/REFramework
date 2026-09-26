#include "RE4TemporalProbe.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/Math.hpp>
#include <sdk/SceneManager.hpp>

namespace {
constexpr std::array<const char*, 5> SCENARIOS{
    "Static screen",
    "Camera pan",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
};

constexpr std::array<const char*, 6> SCENE_INFO_NAMES{
    "main",
    "depthDistortion",
    "filter",
    "jitterDisable",
    "jitterDisablePost",
    "zPrepass",
};

struct ResourceShape {
    uint64_t width{};
    uint32_t height{};
    uint32_t format{};
    uint32_t flags{};
};

const char* scenario_name(int index) {
    if (index < 0 || index >= (int)SCENARIOS.size()) {
        return "Unknown";
    }

    return SCENARIOS[index];
}

ResourceShape resource_shape(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return {};
    }

    const auto desc = resource->GetDesc();
    return {
        .width = desc.Width,
        .height = desc.Height,
        .format = static_cast<uint32_t>(desc.Format),
        .flags = static_cast<uint32_t>(desc.Flags),
    };
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
}

void RE4TemporalProbe::reset_temporal_state() {
    m_temporal_budget.reset();
    m_camera_ptr.store(0, std::memory_order_relaxed);
    m_camera_frame.store(0, std::memory_order_relaxed);
    m_camera_p20.store(0.0f, std::memory_order_relaxed);
    m_camera_p21.store(0.0f, std::memory_order_relaxed);

    m_history_valid.fill(false);
    m_expected_frame = 0;
    m_expected_sample = 0;
    m_expected_matrix_jitter_x = 0.0f;
    m_expected_matrix_jitter_y = 0.0f;
    m_expected_p20.fill(0.0f);
    m_expected_p21.fill(0.0f);
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable deterministic jitter injection test (default off)", &enabled)) {
        reset_temporal_state();
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] deterministic jitter test {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        m_scenario.store(scenario, std::memory_order_relaxed);
    }

    ImGui::Text(
        "Jitter samples: %u / %u",
        m_temporal_budget.sample_count(),
        re4_temporal_probe::MAX_TEMPORAL_SAMPLES);
    ImGui::TextWrapped(
        "RE4-only diagnostic. Injects a 4-phase +/-0.5 pixel jitter pattern into six SceneInfo variants. "
        "No render-size override, XeSS dispatch, or motion-vector readback is active yet.");

    if (ImGui::Button("Reset jitter test")) {
        reset_temporal_state();
    }
}

void RE4TemporalProbe::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (!re4_temporal_probe::should_process_camera_projection(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            camera,
            result)) {
        return;
    }

    auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || camera != reinterpret_cast<REManagedObject*>(primary_camera)) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value()) {
        return;
    }

    m_camera_ptr.store(reinterpret_cast<uintptr_t>(camera), std::memory_order_relaxed);
    m_camera_frame.store(*frame, std::memory_order_relaxed);
    m_camera_p20.store((*result)[2][0], std::memory_order_relaxed);
    m_camera_p21.store((*result)[2][1], std::memory_order_relaxed);
}

void RE4TemporalProbe::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return;
    }

    if (!layer->is_fully_rendered()) {
        return;
    }

    auto* primary_camera = sdk::get_primary_camera();
    auto* scene_camera = layer->get_camera();
    if (primary_camera == nullptr || scene_camera != primary_camera) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value()) {
        return;
    }

    const auto sample = m_temporal_budget.reserve_frame(*frame);
    if (sample == 0) {
        return;
    }

    auto* velocity_resource = layer->get_motion_vectors_d3d12();
    const auto velocity = resource_shape(velocity_resource);
    if (velocity.width == 0 || velocity.height == 0) {
        spdlog::error(
            "[RE4TemporalProbe] jitterSample={} scenario='{}' frame={} missing render extent from VelocityTarget",
            sample,
            scenario_name(m_scenario.load(std::memory_order_relaxed)),
            *frame);
        return;
    }

    auto infos = scene_infos(layer);
    if (infos[0] == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] jitterSample={} scenario='{}' frame={} main SceneInfo is null",
            sample,
            scenario_name(m_scenario.load(std::memory_order_relaxed)),
            *frame);
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    const auto pixel_jitter = re4_temporal_probe::jitter_pixels_for_sample(sample);
    const auto matrix_jitter = re4_temporal_probe::projection_jitter_from_pixels(
        pixel_jitter,
        static_cast<uint32_t>(velocity.width),
        velocity.height);

    const auto camera_frame = m_camera_frame.load(std::memory_order_relaxed);
    const auto camera_same_frame = camera_frame == *frame;
    const auto camera_p20 = m_camera_p20.load(std::memory_order_relaxed);
    const auto camera_p21 = m_camera_p21.load(std::memory_order_relaxed);

    spdlog::info(
        "[RE4TemporalProbe] jitterFrame sample={} scenario='{}' frame={} phase={} "
        "render={}x{} pixel={{x={:.6f},y={:.6f}}} matrix={{x={:.9f},y={:.9f}}} "
        "cameraFrame={} cameraSameFrame={} cameraXY={{p20={:.9f},p21={:.9f}}} "
        "velocityResource={:p} velocityFormat={} velocityFlags=0x{:x}",
        sample,
        scenario,
        *frame,
        (sample - 1) % re4_temporal_probe::JITTER_PHASE_COUNT,
        velocity.width,
        velocity.height,
        pixel_jitter.x,
        pixel_jitter.y,
        matrix_jitter.x,
        matrix_jitter.y,
        camera_frame,
        camera_same_frame,
        camera_p20,
        camera_p21,
        static_cast<void*>(velocity_resource),
        velocity.format,
        velocity.flags);

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            spdlog::info(
                "[RE4TemporalProbe] jitterVariant sample={} scenario='{}' frame={} name={} sceneInfo=null",
                sample,
                scenario,
                *frame,
                SCENE_INFO_NAMES[i]);
            continue;
        }

        const auto current_projection = info->projection_matrix;
        const auto current_view = info->view_matrix;
        const auto before_p20 = current_projection[2][0];
        const auto before_p21 = current_projection[2][1];

        auto previous_projection = m_history_valid[i] ? m_previous_projection[i] : current_projection;
        const auto previous_view = m_history_valid[i] ? m_previous_view[i] : current_view;
        const auto history_was_valid = m_history_valid[i];

        previous_projection[2][0] += matrix_jitter.x;
        previous_projection[2][1] += matrix_jitter.y;
        const auto history_projection_p20 = previous_projection[2][0];
        const auto history_projection_p21 = previous_projection[2][1];

        info->old_view_projection_matrix = previous_projection * previous_view;

        m_previous_projection[i] = current_projection;
        m_previous_view[i] = current_view;
        m_history_valid[i] = true;

        info->projection_matrix[2][0] += matrix_jitter.x;
        info->projection_matrix[2][1] += matrix_jitter.y;
        info->inverse_projection_matrix = glm::inverse(info->projection_matrix);
        info->view_projection_matrix = info->projection_matrix * info->view_matrix;
        info->inverse_view_projection_matrix = glm::inverse(info->view_projection_matrix);

        m_expected_p20[i] = info->projection_matrix[2][0];
        m_expected_p21[i] = info->projection_matrix[2][1];

        spdlog::info(
            "[RE4TemporalProbe] jitterVariant sample={} scenario='{}' frame={} name={} sceneInfo={:p} "
            "historyValid={} before={{p20={:.9f},p21={:.9f}}} "
            "historyProjection={{p20={:.9f},p21={:.9f}}} "
            "after={{p20={:.9f},p21={:.9f}}} oldVP={{m20={:.9f},m21={:.9f}}}",
            sample,
            scenario,
            *frame,
            SCENE_INFO_NAMES[i],
            static_cast<void*>(info),
            history_was_valid,
            before_p20,
            before_p21,
            history_projection_p20,
            history_projection_p21,
            info->projection_matrix[2][0],
            info->projection_matrix[2][1],
            info->old_view_projection_matrix[2][0],
            info->old_view_projection_matrix[2][1]);
    }

    m_expected_frame = *frame;
    m_expected_sample = sample;
    m_expected_matrix_jitter_x = matrix_jitter.x;
    m_expected_matrix_jitter_y = matrix_jitter.y;
}

bool RE4TemporalProbe::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return true;
    }

    if (!layer->is_fully_rendered()) {
        return true;
    }

    auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || layer->get_camera() != primary_camera) {
        return true;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value() || *frame != m_expected_frame || m_expected_sample == 0) {
        return true;
    }

    auto infos = scene_infos(layer);
    bool all_match = true;

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            continue;
        }

        const auto p20_matches = std::abs(info->projection_matrix[2][0] - m_expected_p20[i]) < 0.0000001f;
        const auto p21_matches = std::abs(info->projection_matrix[2][1] - m_expected_p21[i]) < 0.0000001f;
        all_match = all_match && p20_matches && p21_matches;
    }

    spdlog::info(
        "[RE4TemporalProbe] jitterDrawCheck sample={} scenario='{}' frame={} "
        "expectedMatrix={{x={:.9f},y={:.9f}}} allVariantsMatch={} "
        "mainActual={{p20={:.9f},p21={:.9f}}}",
        m_expected_sample,
        scenario_name(m_scenario.load(std::memory_order_relaxed)),
        *frame,
        m_expected_matrix_jitter_x,
        m_expected_matrix_jitter_y,
        all_match,
        infos[0] != nullptr ? infos[0]->projection_matrix[2][0] : 0.0f,
        infos[0] != nullptr ? infos[0]->projection_matrix[2][1] : 0.0f);

    return true;
}
