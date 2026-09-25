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
constexpr uint32_t SAMPLE_INTERVAL_CALLBACKS = 60;
constexpr uint32_t MAX_SAMPLES = 240;
constexpr uint32_t MAX_SCENE_LAYERS = 32;

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

uint32_t reserve_sample(std::atomic<uint32_t>& samples) {
    auto current = samples.load(std::memory_order_relaxed);

    while (current < MAX_SAMPLES) {
        if (samples.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            return current + 1;
        }
    }

    return 0;
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

    ImGui::Text("Scene draw callbacks: %u", m_scene_draw_callbacks.load(std::memory_order_relaxed));
    ImGui::Text("Samples: %u / %u", m_samples.load(std::memory_order_relaxed), MAX_SAMPLES);
    ImGui::TextWrapped("One bounded sample per 60 Scene draw callbacks. Read the REFramework log for candidate object/resource pointers and descriptors.");

    if (ImGui::Button("Reset capture budget")) {
        m_scene_draw_callbacks.store(0, std::memory_order_relaxed);
        m_samples.store(0, std::memory_order_relaxed);
    }
}

void RE4TemporalProbe::on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!m_enabled.load(std::memory_order_relaxed) || layer == nullptr) {
        return;
    }

    const auto callback = m_scene_draw_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (callback % SAMPLE_INTERVAL_CALLBACKS != 0) {
        return;
    }

    const auto sample = reserve_sample(m_samples);
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
    uint32_t scene_layer_count{};
    if (prepare_output_type != nullptr) {
        if (auto* type = prepare_output_type->get_type(); type != nullptr) {
            if (render_output != nullptr) {
                auto& scene_layers = render_output->get_scene_layers();
                auto bounded_scene_layer_count = scene_layers.num < scene_layers.num_allocated
                    ? scene_layers.num
                    : scene_layers.num_allocated;
                if (bounded_scene_layer_count > MAX_SCENE_LAYERS) {
                    bounded_scene_layer_count = MAX_SCENE_LAYERS;
                }
                scene_layer_count = (uint32_t)bounded_scene_layer_count;

                for (uint32_t i = 0; i < scene_layer_count && scene_layers.elements != nullptr; ++i) {
                    auto* candidate_scene = scene_layers.elements[i];
                    if (candidate_scene == nullptr) {
                        continue;
                    }

                    const auto result = candidate_scene->find_layer_recursive(type);
                    if (const auto slot = std::get<1>(result); slot != nullptr) {
                        prepare_output = static_cast<sdk::renderer::layer::PrepareOutput*>(*slot);
                        prepare_parent = std::get<0>(result);
                        break;
                    }
                }
            }

            // PrepareOutput may be a sibling of the Scene layer in the renderer tree.
            if (prepare_output == nullptr) {
                auto* root = sdk::renderer::get_root_layer();
                if (root != nullptr) {
                    const auto result = root->find_layer_recursive(type);
                    if (const auto slot = std::get<1>(result); slot != nullptr) {
                        prepare_output = static_cast<sdk::renderer::layer::PrepareOutput*>(*slot);
                        prepare_parent = std::get<0>(result);
                    }
                }
            }
        }
    }

    auto* output_state = prepare_output != nullptr ? prepare_output->get_output_state() : nullptr;
    auto output_rtv = output_state != nullptr ? output_state->get_rtv(0) : sdk::intrusive_ptr<sdk::renderer::RenderTargetView>{};
    auto* output_texture = output_rtv.has_value() ? output_rtv->get_texture_d3d12().get() : nullptr;
    auto* output_resource = output_state != nullptr ? output_state->get_native_resource_d3d12() : nullptr;

    auto* depth_texture = layer->get_depth_stencil();
    auto* depth_resource = layer->get_depth_stencil_d3d12();
    auto* velocity_state = layer->get_motion_vectors_state();
    auto* velocity_resource = velocity_state != nullptr ? velocity_state->get_native_resource_d3d12() : nullptr;

    spdlog::info(
        "[RE4TemporalProbe] sample={} scenario='{}' frame={} scene={:p} camera={:p} renderOutput={:p} renderOutputSceneLayers={} prepareParent={:p} prepareOutput={:p} outputState={:p} outputRTV0={:p} depthTexture={:p} velocityState={:p}",
        sample,
        scenario,
        frame.has_value() ? std::to_string(*frame) : "unknown",
        static_cast<void*>(layer),
        static_cast<void*>(camera),
        static_cast<void*>(render_output),
        scene_layer_count,
        static_cast<void*>(prepare_parent),
        static_cast<void*>(prepare_output),
        static_cast<void*>(output_state),
        static_cast<void*>(output_rtv.get()),
        static_cast<void*>(depth_texture),
        static_cast<void*>(velocity_state));

    log_resource(sample, scenario, "color_candidate", output_texture, output_resource);
    log_resource(sample, scenario, "depth_candidate", depth_texture, depth_resource);
    log_resource(sample, scenario, "motion_vector_candidate", nullptr, velocity_resource);
}
