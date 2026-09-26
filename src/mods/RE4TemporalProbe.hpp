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
    void on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    void on_application_entry(void* entry, const char* name, size_t hash) override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};
    std::atomic<uint32_t> m_non_primary_scene_callbacks{0};
    std::atomic<uint32_t> m_end_rendering_samples{0};

    re4_temporal_probe::SampleBudget m_sample_budget;
    re4_temporal_probe::SampleBudget m_post_effect_sample_budget;
    re4_temporal_probe::SampleBudget m_overlay_sample_budget;

    std::atomic<uintptr_t> m_last_depth_resource{0};
    std::atomic<uintptr_t> m_last_velocity_resource{0};
    std::array<std::atomic<uintptr_t>, 4> m_last_scene_rtv_resources{};
    std::atomic<uintptr_t> m_last_scene_layer{0};
    std::atomic<uintptr_t> m_last_post_effect_layer{0};
    std::atomic<uintptr_t> m_last_post_effect_target_state{0};
    std::atomic<uintptr_t> m_last_post_effect_texture{0};
    std::atomic<uintptr_t> m_last_post_effect_resource{0};
    std::atomic<uint32_t> m_last_post_effect_frame{0};

    std::atomic<uintptr_t> m_last_overlay_layer{0};
    std::atomic<uintptr_t> m_last_overlay_target_state{0};
    std::atomic<uintptr_t> m_last_overlay_texture{0};
    std::atomic<uintptr_t> m_last_overlay_resource{0};
    std::atomic<uint32_t> m_last_overlay_frame{0};
};
