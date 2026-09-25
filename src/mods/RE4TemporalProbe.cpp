#include "RE4TemporalProbe.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>

namespace {
constexpr uint32_t MAX_SAMPLES = re4_temporal_probe::MAX_SAMPLES;

constexpr std::array<const char*, 5> SCENARIOS{
    "Static screen",
    "Camera pan",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
};

const char* scenario_name(int index) {
    if (index < 0 || index >= (int)SCENARIOS.size()) {
        return "Unknown";
    }

    return SCENARIOS[index];
}

ID3D12Resource* get_native_resource(sdk::renderer::Texture* texture) {
    if (texture == nullptr) {
        return nullptr;
    }

    auto* container = texture->get_d3d12_resource_container();
    return container != nullptr ? container->get_native_resource() : nullptr;
}

void log_resource(
    uint32_t sample,
    const char* scenario,
    const char* role,
    sdk::renderer::Texture* texture,
    ID3D12Resource* resource) {
    if (texture != nullptr) {
        const auto& texture_desc = *texture->get_desc();
        spdlog::info(
            "[RE4TemporalProbe] sample={} scenario='{}' candidate={} engineTexture={:p} engineDesc={{width={},height={},depth={},mips={},array={},format={}}}",
            sample,
            scenario,
            role,
            static_cast<void*>(texture),
            texture_desc.width,
            texture_desc.height,
            texture_desc.depth,
            texture_desc.mip,
            texture_desc.arr,
            texture_desc.format);
    }

    if (resource == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] sample={} scenario='{}' candidate={} nativeResource=null",
            sample,
            scenario,
            role);
        return;
    }

    const auto desc = resource->GetDesc();
    spdlog::info(
        "[RE4TemporalProbe] sample={} scenario='{}' candidate={} nativeResource={:p} d3d12Desc={{dimension={},width={},height={},depthOrArray={},mips={},format={},samples={},flags=0x{:x}}}",
        sample,
        scenario,
        role,
        static_cast<void*>(resource),
        (uint32_t)desc.Dimension,
        desc.Width,
        desc.Height,
        desc.DepthOrArraySize,
        desc.MipLevels,
        (uint32_t)desc.Format,
        desc.SampleDesc.Count,
        (uint32_t)desc.Flags);
}

int32_t matching_scene_rtv_index(
    ID3D12Resource* resource,
    const std::array<std::atomic<uintptr_t>, 4>& scene_resources) {
    if (resource == nullptr) {
        return -1;
    }

    const auto value = reinterpret_cast<uintptr_t>(resource);
    for (size_t i = 0; i < scene_resources.size(); ++i) {
        if (scene_resources[i].load(std::memory_order_relaxed) == value) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable passive capture (default off)", &enabled)) {
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] capture {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        m_scenario.store(scenario, std::memory_order_relaxed);
    }

    ImGui::Text("Primary Scene callbacks: %u", m_sample_budget.callback_count());
    ImGui::Text("Non-primary Scene callbacks: %u", m_non_primary_scene_callbacks.load(std::memory_order_relaxed));
    ImGui::Text("Scene samples: %u / %u", m_sample_budget.sample_count(), MAX_SAMPLES);
    ImGui::Text("PostEffect samples: %u / %u", m_post_effect_sample_budget.sample_count(), MAX_SAMPLES);
    ImGui::Text("Overlay samples: %u / %u", m_overlay_sample_budget.sample_count(), MAX_SAMPLES);
    ImGui::TextWrapped("RE4-only passive probe. The PostEffect R11G11B10_FLOAT target is the current Color candidate; Overlay is sampled separately to prove the pre-HUD boundary.");

    if (ImGui::Button("Reset capture budget")) {
        m_non_primary_scene_callbacks.store(0, std::memory_order_relaxed);
        m_sample_budget.reset();
        m_post_effect_sample_budget.reset();
        m_overlay_sample_budget.reset();
        m_last_depth_resource.store(0, std::memory_order_relaxed);
        m_last_velocity_resource.store(0, std::memory_order_relaxed);
        for (auto& resource : m_last_scene_rtv_resources) {
            resource.store(0, std::memory_order_relaxed);
        }
        m_last_post_effect_resource.store(0, std::memory_order_relaxed);
        m_last_post_effect_frame.store(0, std::memory_order_relaxed);
    }
}

void RE4TemporalProbe::on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return;
    }

    const auto view_id = layer->get_view_id();
    const auto scene_enabled = layer->is_enabled();
    const auto has_main_camera = layer->has_main_camera();
    const auto fully_rendered = layer->is_fully_rendered();
    const auto primary_scene = re4_temporal_probe::is_primary_scene(scene_enabled, has_main_camera, fully_rendered);

    if (!primary_scene) {
        const auto diagnostic_index = m_non_primary_scene_callbacks.fetch_add(1, std::memory_order_relaxed);
        if (diagnostic_index % re4_temporal_probe::SAMPLE_INTERVAL_CALLBACKS == 0 &&
            diagnostic_index / re4_temporal_probe::SAMPLE_INTERVAL_CALLBACKS < re4_temporal_probe::MAX_NON_PRIMARY_SCENE_DIAGNOSTICS) {
            spdlog::info(
                "[RE4TemporalProbe] scene_eligibility scene={:p} viewId={} sceneEnabled={} hasMainCamera={} fullyRendered={} primaryScene=false",
                static_cast<void*>(layer),
                view_id,
                scene_enabled,
                has_main_camera,
                fully_rendered);
        }
        return;
    }

    if (!re4_temporal_probe::should_sample_primary_scene(primary_scene, m_sample_budget)) {
        return;
    }

    const auto sample = m_sample_budget.reserve_sample();
    if (sample == 0) {
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    auto* camera = layer->get_camera();
    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;

    auto* depth_texture = layer->get_depth_stencil();
    auto* depth_resource = layer->get_depth_stencil_d3d12();
    auto* velocity_state = layer->get_motion_vectors_state();
    auto velocity_rtv = velocity_state != nullptr ? velocity_state->get_rtv(0) : sdk::intrusive_ptr<sdk::renderer::RenderTargetView>{};
    auto* velocity_texture = velocity_rtv.has_value() ? velocity_rtv->get_texture_d3d12().get() : nullptr;
    auto* velocity_resource = velocity_state != nullptr ? velocity_state->get_native_resource_d3d12() : nullptr;

    m_last_depth_resource.store(reinterpret_cast<uintptr_t>(depth_resource), std::memory_order_relaxed);
    m_last_velocity_resource.store(reinterpret_cast<uintptr_t>(velocity_resource), std::memory_order_relaxed);
    for (auto& resource : m_last_scene_rtv_resources) {
        resource.store(0, std::memory_order_relaxed);
    }

    spdlog::info(
        "[RE4TemporalProbe] sample={} scenario='{}' frame={} scene={:p} viewId={} sceneEnabled={} hasMainCamera={} fullyRendered={} primaryScene={} camera={:p} renderContext={:p} depthTexture={:p} velocityState={:p} velocityRTV0={:p}",
        sample,
        scenario,
        frame.has_value() ? std::to_string(*frame) : "unknown",
        static_cast<void*>(layer),
        view_id,
        scene_enabled,
        has_main_camera,
        fully_rendered,
        primary_scene,
        static_cast<void*>(camera),
        render_context,
        static_cast<void*>(depth_texture),
        static_cast<void*>(velocity_state),
        static_cast<void*>(velocity_rtv.get()));

    log_resource(sample, scenario, "depth_candidate", depth_texture, depth_resource);
    log_resource(sample, scenario, "motion_vector_candidate", velocity_texture, velocity_resource);

    if (render_context == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] sample={} scenario='{}' sceneTarget renderContext=null",
            sample,
            scenario);
        return;
    }

    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    auto* target_state = context->get_render_target();
    if (target_state == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] sample={} scenario='{}' sceneTarget currentTargetState=null",
            sample,
            scenario);
        return;
    }

    const auto rtv_count = target_state->get_rtv_count();
    const auto bounded_count = (std::min)(rtv_count, re4_temporal_probe::MAX_SCENE_RTVS);

    spdlog::info(
        "[RE4TemporalProbe] sample={} scenario='{}' sceneTarget renderContext={:p} currentTargetState={:p} rtvCount={} boundedCount={}",
        sample,
        scenario,
        render_context,
        static_cast<void*>(target_state),
        rtv_count,
        bounded_count);

    for (uint32_t i = 0; i < bounded_count; ++i) {
        auto rtv = target_state->get_rtv((int32_t)i);
        if (!rtv.has_value()) {
            spdlog::info(
                "[RE4TemporalProbe] sample={} scenario='{}' sceneTarget rtvIndex={} rtv=null",
                sample,
                scenario,
                i);
            continue;
        }

        auto texture = rtv->get_texture_d3d12();
        auto* texture_ptr = texture.get();
        auto* resource = get_native_resource(texture_ptr);

        if (i < m_last_scene_rtv_resources.size()) {
            m_last_scene_rtv_resources[i].store(reinterpret_cast<uintptr_t>(resource), std::memory_order_relaxed);
        }

        const auto& rtv_desc = rtv->get_desc();
        spdlog::info(
            "[RE4TemporalProbe] sample={} scenario='{}' sceneTarget rtvIndex={} rtv={:p} rtvFormat={} rtvDimension={} texture={:p} nativeResource={:p} matchesDepth={} matchesVelocity={}",
            sample,
            scenario,
            i,
            static_cast<void*>(rtv.get()),
            rtv_desc.format,
            rtv_desc.dimension,
            static_cast<void*>(texture_ptr),
            static_cast<void*>(resource),
            resource != nullptr && resource == depth_resource,
            resource != nullptr && resource == velocity_resource);

        if (texture_ptr != nullptr || resource != nullptr) {
            const auto role = i == 0 ? "scene_rtv0_candidate" : "scene_rtv_candidate";
            log_resource(sample, scenario, role, texture_ptr, resource);
        }
    }
}

void RE4TemporalProbe::on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) {
    if (!re4_temporal_probe::should_process_post_effect(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer,
            render_context)) {
        return;
    }

    if (!m_post_effect_sample_budget.should_sample_callback()) {
        return;
    }

    const auto sample = m_post_effect_sample_budget.reserve_sample();
    if (sample == 0) {
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    auto* target_state = context->get_render_target();
    auto* parent = layer->get_parent();

    if (target_state == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] postSample={} scenario='{}' frame={} postEffectTarget layer={:p} parent={:p} renderContext={:p} currentTargetState=null",
            sample,
            scenario,
            frame.has_value() ? std::to_string(*frame) : "unknown",
            static_cast<void*>(layer),
            static_cast<void*>(parent),
            render_context);
        return;
    }

    const auto rtv_count = target_state->get_rtv_count();
    const auto bounded_count = (std::min)(rtv_count, re4_temporal_probe::MAX_POST_EFFECT_RTVS);
    const auto depth_anchor = reinterpret_cast<ID3D12Resource*>(m_last_depth_resource.load(std::memory_order_relaxed));
    const auto velocity_anchor = reinterpret_cast<ID3D12Resource*>(m_last_velocity_resource.load(std::memory_order_relaxed));

    spdlog::info(
        "[RE4TemporalProbe] postSample={} scenario='{}' frame={} postEffectTarget layer={:p} parent={:p} renderContext={:p} currentTargetState={:p} rtvCount={} boundedCount={} depthAnchor={:p} velocityAnchor={:p}",
        sample,
        scenario,
        frame.has_value() ? std::to_string(*frame) : "unknown",
        static_cast<void*>(layer),
        static_cast<void*>(parent),
        render_context,
        static_cast<void*>(target_state),
        rtv_count,
        bounded_count,
        static_cast<void*>(depth_anchor),
        static_cast<void*>(velocity_anchor));

    for (uint32_t i = 0; i < bounded_count; ++i) {
        auto rtv = target_state->get_rtv((int32_t)i);
        if (!rtv.has_value()) {
            spdlog::info(
                "[RE4TemporalProbe] postSample={} scenario='{}' postEffectTarget rtvIndex={} rtv=null",
                sample,
                scenario,
                i);
            continue;
        }

        auto texture = rtv->get_texture_d3d12();
        auto* texture_ptr = texture.get();
        auto* resource = get_native_resource(texture_ptr);
        const auto scene_rtv_index = matching_scene_rtv_index(resource, m_last_scene_rtv_resources);
        const auto& rtv_desc = rtv->get_desc();

        spdlog::info(
            "[RE4TemporalProbe] postSample={} scenario='{}' postEffectTarget rtvIndex={} rtv={:p} rtvFormat={} rtvDimension={} texture={:p} nativeResource={:p} matchesDepth={} matchesVelocity={} matchesSceneRtvIndex={}",
            sample,
            scenario,
            i,
            static_cast<void*>(rtv.get()),
            rtv_desc.format,
            rtv_desc.dimension,
            static_cast<void*>(texture_ptr),
            static_cast<void*>(resource),
            resource != nullptr && resource == depth_anchor,
            resource != nullptr && resource == velocity_anchor,
            scene_rtv_index);

        if (texture_ptr != nullptr || resource != nullptr) {
            log_resource(sample, scenario, "post_effect_rtv_candidate", texture_ptr, resource);
        }

        if (rtv_count == 1 && i == 0 && resource != nullptr) {
            const auto resource_desc = resource->GetDesc();
            if (resource_desc.Width == 2560 &&
                resource_desc.Height == 1440 &&
                resource_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
                m_last_post_effect_resource.store(reinterpret_cast<uintptr_t>(resource), std::memory_order_relaxed);
                m_last_post_effect_frame.store(frame.value_or(0), std::memory_order_relaxed);

                spdlog::info(
                    "[RE4TemporalProbe] postSample={} scenario='{}' postEffectColorAnchor frame={} nativeResource={:p} format={} size={}x{}",
                    sample,
                    scenario,
                    frame.has_value() ? std::to_string(*frame) : "unknown",
                    static_cast<void*>(resource),
                    (uint32_t)resource_desc.Format,
                    resource_desc.Width,
                    resource_desc.Height);
            }
        }
    }
}

void RE4TemporalProbe::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    if (!re4_temporal_probe::should_process_overlay(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer,
            render_context)) {
        return;
    }

    if (!m_overlay_sample_budget.should_sample_callback()) {
        return;
    }

    const auto sample = m_overlay_sample_budget.reserve_sample();
    if (sample == 0) {
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    auto* target_state = context->get_render_target();
    auto* parent = layer->get_parent();

    const auto post_effect_resource =
        reinterpret_cast<ID3D12Resource*>(m_last_post_effect_resource.load(std::memory_order_relaxed));
    const auto post_effect_frame = m_last_post_effect_frame.load(std::memory_order_relaxed);
    const auto same_frame = frame.has_value() && post_effect_frame != 0 && *frame == post_effect_frame;
    const auto depth_anchor =
        reinterpret_cast<ID3D12Resource*>(m_last_depth_resource.load(std::memory_order_relaxed));
    const auto velocity_anchor =
        reinterpret_cast<ID3D12Resource*>(m_last_velocity_resource.load(std::memory_order_relaxed));

    if (target_state == nullptr) {
        spdlog::info(
            "[RE4TemporalProbe] overlaySample={} scenario='{}' frame={} overlayTarget layer={:p} parent={:p} renderContext={:p} currentTargetState=null postEffectFrame={} sameFrame={} postEffectResource={:p}",
            sample,
            scenario,
            frame.has_value() ? std::to_string(*frame) : "unknown",
            static_cast<void*>(layer),
            static_cast<void*>(parent),
            render_context,
            post_effect_frame,
            same_frame,
            static_cast<void*>(post_effect_resource));
        return;
    }

    const auto rtv_count = target_state->get_rtv_count();
    const auto bounded_count = (std::min)(rtv_count, re4_temporal_probe::MAX_OVERLAY_RTVS);

    spdlog::info(
        "[RE4TemporalProbe] overlaySample={} scenario='{}' frame={} overlayTarget layer={:p} parent={:p} renderContext={:p} currentTargetState={:p} rtvCount={} boundedCount={} postEffectFrame={} sameFrame={} postEffectResource={:p}",
        sample,
        scenario,
        frame.has_value() ? std::to_string(*frame) : "unknown",
        static_cast<void*>(layer),
        static_cast<void*>(parent),
        render_context,
        static_cast<void*>(target_state),
        rtv_count,
        bounded_count,
        post_effect_frame,
        same_frame,
        static_cast<void*>(post_effect_resource));

    for (uint32_t i = 0; i < bounded_count; ++i) {
        auto rtv = target_state->get_rtv((int32_t)i);
        if (!rtv.has_value()) {
            spdlog::info(
                "[RE4TemporalProbe] overlaySample={} scenario='{}' overlayTarget rtvIndex={} rtv=null",
                sample,
                scenario,
                i);
            continue;
        }

        auto texture = rtv->get_texture_d3d12();
        auto* texture_ptr = texture.get();
        auto* resource = get_native_resource(texture_ptr);
        const auto scene_rtv_index = matching_scene_rtv_index(resource, m_last_scene_rtv_resources);
        const auto& rtv_desc = rtv->get_desc();

        spdlog::info(
            "[RE4TemporalProbe] overlaySample={} scenario='{}' overlayTarget rtvIndex={} rtv={:p} rtvFormat={} rtvDimension={} texture={:p} nativeResource={:p} matchesPostEffect={} matchesDepth={} matchesVelocity={} matchesSceneRtvIndex={}",
            sample,
            scenario,
            i,
            static_cast<void*>(rtv.get()),
            rtv_desc.format,
            rtv_desc.dimension,
            static_cast<void*>(texture_ptr),
            static_cast<void*>(resource),
            resource != nullptr && resource == post_effect_resource,
            resource != nullptr && resource == depth_anchor,
            resource != nullptr && resource == velocity_anchor,
            scene_rtv_index);

        if (texture_ptr != nullptr || resource != nullptr) {
            log_resource(sample, scenario, "overlay_rtv_candidate", texture_ptr, resource);
        }
    }
}
