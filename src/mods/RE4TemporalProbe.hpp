#pragma once

#include <atomic>

#include <sdk/Renderer.hpp>

#include "Mod.hpp"
#include "RE4TemporalProbeSupport.hpp"

class RE4TemporalProbe final : public Mod {
public:
    std::string_view get_name() const override { return "RE4TemporalProbe"; }

    void on_draw_ui() override;
    void on_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};
    re4_temporal_probe::SampleBudget m_sample_budget;
};
