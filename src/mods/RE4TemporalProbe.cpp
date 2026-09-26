#include "RE4TemporalProbe.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/SceneManager.hpp>

namespace {
constexpr std::array<const char*, 5> SCENARIOS{
    "Static screen",
    "Camera pan",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
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

void log_scene_info_offsets(
    uint32_t sample,
    uint32_t frame,
    const char* scenario,
    sdk::renderer::SceneInfo* main,
    sdk::renderer::SceneInfo* depth_distortion,
    sdk::renderer::SceneInfo* filter,
    sdk::renderer::SceneInfo* jitter_disable,
    sdk::renderer::SceneInfo* jitter_disable_post,
    sdk::renderer::SceneInfo* z_prepass) {
    const auto p20 = [](sdk::renderer::SceneInfo* info) {
        return info != nullptr ? info->projection_matrix[2][0] : 0.0f;
    };
    const auto p21 = [](sdk::renderer::SceneInfo* info) {
        return info != nullptr ? info->projection_matrix[2][1] : 0.0f;
    };

    spdlog::info(
        "[RE4TemporalProbe] temporalSample={} scenario='{}' frame={} sceneInfoOffsets "
        "main={:p}:{:.9f},{:.9f} depthDistortion={:p}:{:.9f},{:.9f} "
        "filter={:p}:{:.9f},{:.9f} jitterDisable={:p}:{:.9f},{:.9f} "
        "jitterDisablePost={:p}:{:.9f},{:.9f} zPrepass={:p}:{:.9f},{:.9f}",
        sample,
        scenario,
        frame,
        static_cast<void*>(main), p20(main), p21(main),
        static_cast<void*>(depth_distortion), p20(depth_distortion), p21(depth_distortion),
        static_cast<void*>(filter), p20(filter), p21(filter),
        static_cast<void*>(jitter_disable), p20(jitter_disable), p21(jitter_disable),
        static_cast<void*>(jitter_disable_post), p20(jitter_disable_post), p21(jitter_disable_post),
        static_cast<void*>(z_prepass), p20(z_prepass), p21(z_prepass));
}
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable jitter / MV semantic capture (default off)", &enabled)) {
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] temporal capture {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        m_scenario.store(scenario, std::memory_order_relaxed);
    }

    ImGui::Text(
        "Temporal samples: %u / %u",
        m_temporal_budget.sample_count(),
        re4_temporal_probe::MAX_TEMPORAL_SAMPLES);
    ImGui::TextWrapped(
        "RE4-only consecutive-frame diagnostic. No render-size override is active. "
        "It compares the primary Camera projection with SceneInfo projection offsets and records VelocityTarget metadata.");

    if (ImGui::Button("Reset temporal capture")) {
        m_temporal_budget.reset();
        m_camera_ptr.store(0, std::memory_order_relaxed);
        m_camera_frame.store(0, std::memory_order_relaxed);
        m_previous_scene_projection_valid = false;
        m_previous_scene_p20 = 0.0f;
        m_previous_scene_p21 = 0.0f;
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
    m_camera_p00.store((*result)[0][0], std::memory_order_relaxed);
    m_camera_p11.store((*result)[1][1], std::memory_order_relaxed);
    m_camera_p20.store((*result)[2][0], std::memory_order_relaxed);
    m_camera_p21.store((*result)[2][1], std::memory_order_relaxed);
    m_camera_p22.store((*result)[2][2], std::memory_order_relaxed);
    m_camera_p23.store((*result)[2][3], std::memory_order_relaxed);
    m_camera_p32.store((*result)[3][2], std::memory_order_relaxed);
    m_camera_p33.store((*result)[3][3], std::memory_order_relaxed);
}

void RE4TemporalProbe::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!re4_temporal_probe::should_process_scene_update(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return;
    }

    if (!layer->is_fully_rendered()) {
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

    auto* scene_info = layer->get_scene_info();
    if (scene_info == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] temporalSample={} scenario='{}' frame={} sceneInfo=null",
            sample,
            scenario_name(m_scenario.load(std::memory_order_relaxed)),
            *frame);
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    auto* scene_camera = layer->get_camera();
    auto* velocity_resource = layer->get_motion_vectors_d3d12();
    const auto velocity = resource_shape(velocity_resource);

    const auto scene_p00 = scene_info->projection_matrix[0][0];
    const auto scene_p11 = scene_info->projection_matrix[1][1];
    const auto scene_p20 = scene_info->projection_matrix[2][0];
    const auto scene_p21 = scene_info->projection_matrix[2][1];
    const auto scene_p22 = scene_info->projection_matrix[2][2];
    const auto scene_p23 = scene_info->projection_matrix[2][3];
    const auto scene_p32 = scene_info->projection_matrix[3][2];
    const auto scene_p33 = scene_info->projection_matrix[3][3];

    const auto previous_delta_p20 =
        m_previous_scene_projection_valid ? scene_p20 - m_previous_scene_p20 : 0.0f;
    const auto previous_delta_p21 =
        m_previous_scene_projection_valid ? scene_p21 - m_previous_scene_p21 : 0.0f;

    m_previous_scene_projection_valid = true;
    m_previous_scene_p20 = scene_p20;
    m_previous_scene_p21 = scene_p21;

    const auto camera_ptr = m_camera_ptr.load(std::memory_order_relaxed);
    const auto camera_frame = m_camera_frame.load(std::memory_order_relaxed);
    const auto camera_same_frame = camera_frame == *frame;
    const auto camera_matches_scene =
        camera_ptr != 0 &&
        camera_ptr == reinterpret_cast<uintptr_t>(scene_camera);

    const auto camera_p20 = m_camera_p20.load(std::memory_order_relaxed);
    const auto camera_p21 = m_camera_p21.load(std::memory_order_relaxed);

    spdlog::info(
        "[RE4TemporalProbe] temporalSample={} scenario='{}' frame={} scene={:p} camera={:p} "
        "projection={{p00={:.9f},p11={:.9f},p20={:.9f},p21={:.9f},p22={:.9f},p23={:.9f},p32={:.9f},p33={:.9f}}} "
        "frameDelta={{p20={:.9f},p21={:.9f}}} oldVP={{m20={:.9f},m21={:.9f}}}",
        sample,
        scenario,
        *frame,
        static_cast<void*>(layer),
        static_cast<void*>(scene_camera),
        scene_p00,
        scene_p11,
        scene_p20,
        scene_p21,
        scene_p22,
        scene_p23,
        scene_p32,
        scene_p33,
        previous_delta_p20,
        previous_delta_p21,
        scene_info->old_view_projection_matrix[2][0],
        scene_info->old_view_projection_matrix[2][1]);

    spdlog::info(
        "[RE4TemporalProbe] temporalSample={} scenario='{}' frame={} cameraProjection "
        "cameraPtr={:p} cameraFrame={} sameFrame={} matchesSceneCamera={} "
        "projection={{p00={:.9f},p11={:.9f},p20={:.9f},p21={:.9f},p22={:.9f},p23={:.9f},p32={:.9f},p33={:.9f}}} "
        "sceneMinusCamera={{p20={:.9f},p21={:.9f}}}",
        sample,
        scenario,
        *frame,
        reinterpret_cast<void*>(camera_ptr),
        camera_frame,
        camera_same_frame,
        camera_matches_scene,
        m_camera_p00.load(std::memory_order_relaxed),
        m_camera_p11.load(std::memory_order_relaxed),
        camera_p20,
        camera_p21,
        m_camera_p22.load(std::memory_order_relaxed),
        m_camera_p23.load(std::memory_order_relaxed),
        m_camera_p32.load(std::memory_order_relaxed),
        m_camera_p33.load(std::memory_order_relaxed),
        scene_p20 - camera_p20,
        scene_p21 - camera_p21);

    spdlog::info(
        "[RE4TemporalProbe] temporalSample={} scenario='{}' frame={} velocityResource={:p} "
        "velocity={{width={},height={},format={},flags=0x{:x}}}",
        sample,
        scenario,
        *frame,
        static_cast<void*>(velocity_resource),
        velocity.width,
        velocity.height,
        velocity.format,
        velocity.flags);

    log_scene_info_offsets(
        sample,
        *frame,
        scenario,
        scene_info,
        layer->get_depth_distortion_scene_info(),
        layer->get_filter_scene_info(),
        layer->get_jitter_disable_scene_info(),
        layer->get_jitter_disable_post_scene_info(),
        layer->get_z_prepass_scene_info());
}
