#include "RE4TemporalProbe.hpp"

#include <array>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/RETypeDefinition.hpp>
#include <sdk/types/REComponent.hpp>

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
    ImGui::Text("Samples: %u / %u", m_sample_budget.sample_count(), MAX_SAMPLES);
    ImGui::TextWrapped("One bounded sample per 60 primary Scene draw callbacks. Read the REFramework log for candidate object/resource pointers and descriptors.");

    if (ImGui::Button("Reset capture budget")) {
        m_non_primary_scene_callbacks.store(0, std::memory_order_relaxed);
        m_sample_budget.reset();
    }
}

void RE4TemporalProbe::on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!re4_temporal_probe::should_process_scene(m_enabled.load(std::memory_order_relaxed), layer)) {
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

    static auto render_output_type = sdk::find_type_definition("via.render.RenderOutput");
    static auto prepare_output_type = sdk::find_type_definition("via.render.layer.PrepareOutput");

    sdk::renderer::RenderOutput* render_output{};
    if (camera != nullptr && render_output_type != nullptr) {
        if (auto* type = render_output_type->get_type(); type != nullptr) {
            render_output = camera->find<sdk::renderer::RenderOutput>(type);
        }
    }

    sdk::renderer::layer::PrepareOutput* prepare_output{};
    sdk::renderer::RenderLayer* prepare_parent{};
    sdk::renderer::layer::PrepareOutput* recursive_prepare_fallback{};
    sdk::renderer::RenderLayer* recursive_prepare_parent{};
    if (primary_scene && prepare_output_type != nullptr) {
        if (auto* type = prepare_output_type->get_type(); type != nullptr) {
            if (auto* slot = layer->find_layer(type); slot != nullptr && *slot != nullptr) {
                prepare_output = static_cast<sdk::renderer::layer::PrepareOutput*>(*slot);
                prepare_parent = layer;
            }
            if (!re4_temporal_probe::has_scene_local_color_candidate(primary_scene, prepare_output)) {
                const auto result = layer->find_layer_recursive(type);
                if (const auto slot = std::get<1>(result); slot != nullptr && *slot != nullptr) {
                    recursive_prepare_fallback = static_cast<sdk::renderer::layer::PrepareOutput*>(*slot);
                    recursive_prepare_parent = std::get<0>(result);
                }
            }
        }
    }

    auto* output_state = prepare_output != nullptr ? prepare_output->get_output_state() : nullptr;
    auto output_rtv = output_state != nullptr ? output_state->get_rtv(0) : sdk::intrusive_ptr<sdk::renderer::RenderTargetView>{};
    auto* output_texture = output_rtv.has_value() ? output_rtv->get_texture_d3d12().get() : nullptr;
    auto* output_resource = output_state != nullptr ? output_state->get_native_resource_d3d12() : nullptr;

    auto* recursive_fallback_state = recursive_prepare_fallback != nullptr ? recursive_prepare_fallback->get_output_state() : nullptr;
    auto recursive_fallback_rtv = recursive_fallback_state != nullptr ? recursive_fallback_state->get_rtv(0) : sdk::intrusive_ptr<sdk::renderer::RenderTargetView>{};
    auto* recursive_fallback_texture = recursive_fallback_rtv.has_value() ? recursive_fallback_rtv->get_texture_d3d12().get() : nullptr;
    auto* recursive_fallback_resource = recursive_fallback_state != nullptr ? recursive_fallback_state->get_native_resource_d3d12() : nullptr;

    auto* depth_texture = primary_scene ? layer->get_depth_stencil() : nullptr;
    auto* depth_resource = primary_scene ? layer->get_depth_stencil_d3d12() : nullptr;
    auto* velocity_state = primary_scene ? layer->get_motion_vectors_state() : nullptr;
    auto velocity_rtv = velocity_state != nullptr ? velocity_state->get_rtv(0) : sdk::intrusive_ptr<sdk::renderer::RenderTargetView>{};
    auto* velocity_texture = velocity_rtv.has_value() ? velocity_rtv->get_texture_d3d12().get() : nullptr;
    auto* velocity_resource = velocity_state != nullptr ? velocity_state->get_native_resource_d3d12() : nullptr;

    spdlog::info(
        "[RE4TemporalProbe] sample={} scenario='{}' frame={} scene={:p} viewId={} sceneEnabled={} hasMainCamera={} fullyRendered={} primaryScene={} camera={:p} renderOutput={:p} prepareParent={:p} prepareOutput={:p} recursivePrepareParent={:p} recursivePrepareFallback={:p} outputState={:p} outputRTV0={:p} depthTexture={:p} velocityState={:p} velocityRTV0={:p}",
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
        static_cast<void*>(render_output),
        static_cast<void*>(prepare_parent),
        static_cast<void*>(prepare_output),
        static_cast<void*>(recursive_prepare_parent),
        static_cast<void*>(recursive_prepare_fallback),
        static_cast<void*>(output_state),
        static_cast<void*>(output_rtv.get()),
        static_cast<void*>(depth_texture),
        static_cast<void*>(velocity_state),
        static_cast<void*>(velocity_rtv.get()));

    log_resource(sample, scenario, "color_candidate", output_texture, output_resource);
    if (recursive_prepare_fallback != nullptr) {
        log_resource(sample, scenario, "color_candidate_recursive_fallback", recursive_fallback_texture, recursive_fallback_resource);
    }
    log_resource(sample, scenario, "depth_candidate", depth_texture, depth_resource);
    log_resource(sample, scenario, "motion_vector_candidate", velocity_texture, velocity_resource);
}
