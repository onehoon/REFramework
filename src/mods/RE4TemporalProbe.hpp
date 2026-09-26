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
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};

    re4_temporal_probe::FrameBudget m_temporal_budget;

    std::atomic<uintptr_t> m_camera_ptr{0};
    std::atomic<uint32_t> m_camera_frame{0};
    std::atomic<float> m_camera_p00{0.0f};
    std::atomic<float> m_camera_p11{0.0f};
    std::atomic<float> m_camera_p20{0.0f};
    std::atomic<float> m_camera_p21{0.0f};
    std::atomic<float> m_camera_p22{0.0f};
    std::atomic<float> m_camera_p23{0.0f};
    std::atomic<float> m_camera_p32{0.0f};
    std::atomic<float> m_camera_p33{0.0f};

    bool m_previous_scene_projection_valid{false};
    float m_previous_scene_p20{0.0f};
    float m_previous_scene_p21{0.0f};
};
