#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <wrl.h>
#include <d3d12.h>

#include <sdk/Renderer.hpp>

#include "Mod.hpp"
#include "RE4TemporalProbeSupport.hpp"

class RE4TemporalProbe final : public Mod {
public:
    std::string_view get_name() const override { return "RE4TemporalProbe"; }

    void on_draw_ui() override;
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
    void on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;
    void on_present() override;
    void on_device_reset() override;

private:
    static constexpr size_t SCENE_INFO_COUNT = 6;

    void reset_temporal_state();
    bool ensure_mv_readback_resources();
    void release_mv_readback_resources();
    void perform_mv_readback();

    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};

    re4_temporal_probe::FrameBudget m_temporal_budget;
    re4_temporal_probe::FrameBudget m_reset_watch_budget;

    bool m_reset_witness_valid{false};
    uint32_t m_reset_previous_frame{0};
    uintptr_t m_reset_previous_scene{0};
    uintptr_t m_reset_previous_scene_info{0};
    uintptr_t m_reset_previous_camera{0};
    uintptr_t m_reset_previous_depth{0};
    uintptr_t m_reset_previous_velocity{0};
    uintptr_t m_reset_previous_color{0};
    uint32_t m_reset_previous_width{0};
    uint32_t m_reset_previous_height{0};
    Matrix4x4f m_reset_previous_view{};

    std::atomic<uintptr_t> m_camera_ptr{0};
    std::atomic<uint32_t> m_camera_frame{0};
    std::atomic<float> m_camera_p20{0.0f};
    std::atomic<float> m_camera_p21{0.0f};
    std::atomic<float> m_camera_p22{0.0f};
    std::atomic<float> m_camera_p23{0.0f};
    std::atomic<float> m_camera_p32{0.0f};
    std::atomic<float> m_camera_p33{0.0f};
    std::atomic<float> m_camera_near{0.0f};
    std::atomic<float> m_camera_far{0.0f};
    std::atomic<bool> m_camera_clip_valid{false};

    std::array<Matrix4x4f, SCENE_INFO_COUNT> m_previous_projection{};
    std::array<Matrix4x4f, SCENE_INFO_COUNT> m_previous_view{};
    std::array<uint32_t, SCENE_INFO_COUNT> m_previous_scene_frame{};
    std::array<bool, SCENE_INFO_COUNT> m_history_valid{};

    uint32_t m_expected_frame{0};
    uint32_t m_expected_sample{0};
    float m_expected_matrix_jitter_x{0.0f};
    float m_expected_matrix_jitter_y{0.0f};
    std::array<float, SCENE_INFO_COUNT> m_expected_p20{};
    std::array<float, SCENE_INFO_COUNT> m_expected_p21{};
    std::array<re4_temporal_probe::MotionPixelCandidate, re4_temporal_probe::MV_SAMPLE_POINT_COUNT>
        m_expected_rotation_reprojection{};
    uint32_t m_expected_rotation_reprojection_previous_frame{0};
    uint32_t m_expected_rotation_reprojection_frame{0};
    bool m_expected_rotation_reprojection_valid{false};

    sdk::intrusive_ptr<sdk::renderer::Texture> m_velocity_copy{};
    uint32_t m_velocity_copy_sample{0};
    uint32_t m_velocity_copy_frame{0};
    uint32_t m_velocity_copy_width{0};
    uint32_t m_velocity_copy_height{0};
    int m_velocity_copy_scenario{0};
    std::array<re4_temporal_probe::MotionPixelCandidate, re4_temporal_probe::MV_SAMPLE_POINT_COUNT>
        m_velocity_copy_rotation_reprojection{};
    uint32_t m_velocity_copy_rotation_reprojection_previous_frame{0};
    bool m_velocity_copy_rotation_reprojection_valid{false};
    bool m_velocity_copy_ready{false};
    bool m_mv_readback_failed{false};

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_mv_command_allocator{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_mv_command_list{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_mv_readback_buffer{};
    Microsoft::WRL::ComPtr<ID3D12Fence> m_mv_fence{};
    UINT64 m_mv_fence_value{0};
    HANDLE m_mv_fence_event{nullptr};
};
