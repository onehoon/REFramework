#pragma once

#include <atomic>
#include <cstdint>

#include <sdk/Renderer.hpp>

#include "Mod.hpp"
#include "RE4TemporalProbeSupport.hpp"

class RE4TemporalProbe final : public Mod {
public:
    std::string_view get_name() const override { return "RE4TemporalProbe"; }

    void on_draw_ui() override;
    void on_view_get_size(REManagedObject* scene_view, float* result) override;
    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    void on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};

    re4_temporal_probe::SampleBudget m_size_sample_budget;

    std::atomic<uintptr_t> m_latest_scene_view{0};
    std::atomic<uint32_t> m_latest_view_frame{0};
    std::atomic<uint32_t> m_latest_view_width{0};
    std::atomic<uint32_t> m_latest_view_height{0};
    std::atomic<uint32_t> m_latest_original_view_width{0};
    std::atomic<uint32_t> m_latest_original_view_height{0};

    std::atomic<uint32_t> m_size_pair_sample{0};
    std::atomic<uint32_t> m_size_pair_frame{0};
};
