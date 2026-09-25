#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <sdk/Renderer.hpp>

#include "Mod.hpp"
#include "RE4TemporalProbeSupport.hpp"

class RE4TemporalProbe final : public Mod {
public:
    std::string_view get_name() const override { return "RE4TemporalProbe"; }

    void on_draw_ui() override;
    void on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};
    std::atomic<uint32_t> m_non_primary_scene_callbacks{0};

    re4_temporal_probe::SampleBudget m_sample_budget;
    re4_temporal_probe::SampleBudget m_post_effect_sample_budget;

    std::atomic<uintptr_t> m_last_depth_resource{0};
    std::atomic<uintptr_t> m_last_velocity_resource{0};
    std::array<std::atomic<uintptr_t>, 4> m_last_scene_rtv_resources{};
};
