#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <wrl.h>
#include <d3d12.h>

#include <sdk/Renderer.hpp>

#include "Mod.hpp"
#include "RE4TemporalProbeSupport.hpp"
#include "utility/VtableHook.hpp"
#include "utility/FunctionHook.hpp"

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
    void log_command_list_interfaces(ID3D12CommandList* command_list);
    bool ensure_recording_function_hooks(ID3D12CommandList* command_list);
    void release_recording_function_hooks();
    void refresh_output_copy_swapchain_buffers();
    bool ensure_bridge_order_resources();
    void release_bridge_order_resources();
    bool submit_bridge_order_empty_list(uint32_t sample, uint32_t frame);
    bool is_bridge_order_list(ID3D12CommandList* command_list) const;
    bool ensure_execution_queue_hook();
    void release_execution_queue_hook();
    bool ensure_resource_command_list_hook(ID3D12GraphicsCommandList* command_list);
    void release_resource_command_list_hooks();
    static void STDMETHODCALLTYPE execute_command_lists_hook(
        ID3D12CommandQueue* queue,
        UINT num_command_lists,
        ID3D12CommandList* const* command_lists);
    static HRESULT STDMETHODCALLTYPE command_list_close_hook(
        ID3D12GraphicsCommandList* command_list);
    static HRESULT STDMETHODCALLTYPE command_list_reset_hook(
        ID3D12GraphicsCommandList* command_list,
        ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initial_state);
    static void STDMETHODCALLTYPE command_list_resource_barrier_hook(
        ID3D12GraphicsCommandList* command_list,
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER* barriers);
    static HRESULT STDMETHODCALLTYPE recording_close_hook(
        ID3D12GraphicsCommandList* command_list);
    static HRESULT STDMETHODCALLTYPE recording_reset_hook(
        ID3D12GraphicsCommandList* command_list,
        ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initial_state);
    static void STDMETHODCALLTYPE recording_resource_barrier_hook(
        ID3D12GraphicsCommandList* command_list,
        UINT num_barriers,
        const D3D12_RESOURCE_BARRIER* barriers);
    static void STDMETHODCALLTYPE recording_enhanced_barrier_hook(
        ID3D12GraphicsCommandList7* command_list,
        UINT32 num_barrier_groups,
        const D3D12_BARRIER_GROUP* barrier_groups);
    static void STDMETHODCALLTYPE recording_copy_texture_region_hook(
        ID3D12GraphicsCommandList* command_list,
        const D3D12_TEXTURE_COPY_LOCATION* dst,
        UINT dst_x,
        UINT dst_y,
        UINT dst_z,
        const D3D12_TEXTURE_COPY_LOCATION* src,
        const D3D12_BOX* src_box);
    static void STDMETHODCALLTYPE recording_copy_resource_hook(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* dst,
        ID3D12Resource* src);

    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_scenario{0};

    re4_temporal_probe::FrameBudget m_temporal_budget;
    re4_temporal_probe::FrameBudget m_reset_watch_budget;
    re4_temporal_probe::FrameBudget m_load_state_budget;
    re4_temporal_probe::FrameBudget m_execution_order_budget;
    re4_temporal_probe::FrameBudget m_resource_state_budget;
    re4_temporal_probe::FrameBudget m_interface_provenance_budget;
    re4_temporal_probe::FrameBudget m_recording_function_budget;
    re4_temporal_probe::FrameBudget m_bridge_order_budget;
    re4_temporal_probe::FrameBudget m_output_copy_budget;

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
    Matrix4x4f m_reset_previous_view_projection{};
    bool m_reset_previous_view_projection_valid{false};

    bool m_load_state_witness_valid{false};
    uint32_t m_load_state_previous_frame{0};
    Matrix4x4f m_load_state_previous_view{};
    std::unordered_map<std::string, uintptr_t> m_load_state_objects{};
    std::unordered_map<std::string, uint64_t> m_load_state_values{};
    std::unordered_set<std::string> m_load_state_schema_keys{};

    using ExecuteCommandListsFn = void (STDMETHODCALLTYPE*)(
        ID3D12CommandQueue*,
        UINT,
        ID3D12CommandList* const*);

    using CommandListCloseFn = HRESULT (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*);
    using CommandListResetFn = HRESULT (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*,
        ID3D12CommandAllocator*,
        ID3D12PipelineState*);
    using CommandListResourceBarrierFn = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*,
        UINT,
        const D3D12_RESOURCE_BARRIER*);

    using CommandListEnhancedBarrierFn = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList7*,
        UINT32,
        const D3D12_BARRIER_GROUP*);
    using CommandListCopyTextureRegionFn = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*,
        const D3D12_TEXTURE_COPY_LOCATION*,
        UINT,
        UINT,
        UINT,
        const D3D12_TEXTURE_COPY_LOCATION*,
        const D3D12_BOX*);
    using CommandListCopyResourceFn = void (STDMETHODCALLTYPE*)(
        ID3D12GraphicsCommandList*,
        ID3D12Resource*,
        ID3D12Resource*);

    struct ResourceCommandListHookState {
        std::unique_ptr<VtableHook> hook{};
        CommandListCloseFn close_original{nullptr};
        CommandListResetFn reset_original{nullptr};
        CommandListResourceBarrierFn barrier_original{nullptr};
        uint64_t generation{0};
        uint64_t target_barrier_sequence{0};
    };

    static inline RE4TemporalProbe* s_execution_probe_instance{nullptr};
    std::unique_ptr<VtableHook> m_execution_queue_hook{};
    ExecuteCommandListsFn m_execution_queue_original{nullptr};
    std::atomic<uint32_t> m_execution_boundary_frame{0};
    std::atomic<uint32_t> m_execution_boundary_sample{0};
    std::atomic<uint64_t> m_execution_submit_count{0};
    std::atomic<uint64_t> m_execution_boundary_submit_base{0};

    std::mutex m_resource_state_mutex{};
    std::unordered_map<uintptr_t, ResourceCommandListHookState> m_resource_command_list_hooks{};
    std::unordered_map<uint32_t, std::pair<uintptr_t, uint64_t>> m_resource_active_by_thread{};
    std::atomic<uint32_t> m_resource_boundary_frame{0};
    std::atomic<uint32_t> m_resource_boundary_sample{0};
    std::atomic<uint64_t> m_resource_submit_count{0};
    std::atomic<uint64_t> m_resource_boundary_submit_base{0};
    std::atomic<uint64_t> m_resource_event_sequence{0};
    std::atomic<uintptr_t> m_resource_color{0};
    std::atomic<uintptr_t> m_resource_depth{0};
    std::atomic<uintptr_t> m_resource_velocity{0};

    std::mutex m_interface_provenance_mutex{};
    std::unordered_set<uintptr_t> m_interface_logged_lists{};
    std::atomic<bool> m_interface_capture_open{false};
    std::atomic<uint32_t> m_interface_boundary_frame{0};
    std::atomic<uint32_t> m_interface_boundary_sample{0};
    uint32_t m_interface_last_submit_frame{0};
    uint32_t m_interface_submit_ordinal{0};

    struct RecordingListState {
        uint64_t generation{0};
        uint64_t target_barrier_sequence{0};
        uint64_t legacy_barrier_calls{0};
        uint64_t enhanced_barrier_calls{0};
    };

    std::mutex m_recording_mutex{};
    std::unordered_set<uintptr_t> m_recording_tracked_lists{};
    std::unordered_map<uintptr_t, RecordingListState> m_recording_list_states{};
    std::unordered_map<uint32_t, std::pair<uintptr_t, uint64_t>> m_recording_active_by_thread{};
    std::unique_ptr<FunctionHook> m_recording_close_hook{};
    std::unique_ptr<FunctionHook> m_recording_reset_hook{};
    std::unique_ptr<FunctionHook> m_recording_resource_barrier_hook{};
    std::unique_ptr<FunctionHook> m_recording_enhanced_barrier_hook{};
    std::unique_ptr<FunctionHook> m_recording_copy_texture_region_hook{};
    std::unique_ptr<FunctionHook> m_recording_copy_resource_hook{};
    CommandListCloseFn m_recording_close_original{nullptr};
    CommandListResetFn m_recording_reset_original{nullptr};
    CommandListResourceBarrierFn m_recording_resource_barrier_original{nullptr};
    CommandListEnhancedBarrierFn m_recording_enhanced_barrier_original{nullptr};
    CommandListCopyTextureRegionFn m_recording_copy_texture_region_original{nullptr};
    CommandListCopyResourceFn m_recording_copy_resource_original{nullptr};
    std::atomic<bool> m_recording_hooks_ready{false};
    std::atomic<bool> m_recording_capture_open{false};
    std::atomic<uint32_t> m_recording_boundary_frame{0};
    std::atomic<uint32_t> m_recording_boundary_sample{0};
    std::atomic<uint64_t> m_recording_event_sequence{0};
    std::atomic<uintptr_t> m_recording_color{0};
    std::atomic<uintptr_t> m_recording_depth{0};
    std::atomic<uintptr_t> m_recording_velocity{0};
    uint32_t m_recording_last_submit_frame{0};
    uint32_t m_recording_submit_ordinal{0};

    struct BridgeOrderSlot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator{};
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list{};
        UINT64 fence_value{0};
    };

    std::mutex m_bridge_order_mutex{};
    std::array<BridgeOrderSlot, re4_temporal_probe::BRIDGE_ORDER_SLOT_COUNT>
        m_bridge_order_slots{};
    Microsoft::WRL::ComPtr<ID3D12Fence> m_bridge_order_fence{};
    UINT64 m_bridge_order_next_fence_value{0};
    uint32_t m_bridge_order_next_slot{0};
    std::atomic<bool> m_bridge_order_capture_open{false};
    std::atomic<uint32_t> m_bridge_order_boundary_frame{0};
    std::atomic<uint32_t> m_bridge_order_boundary_sample{0};
    std::atomic<uint64_t> m_bridge_order_submitted_count{0};
    std::atomic<uint64_t> m_bridge_order_skipped_count{0};
    uint32_t m_bridge_order_last_submit_frame{0};
    uint32_t m_bridge_order_submit_ordinal{0};

    std::mutex m_output_copy_mutex{};
    std::unordered_set<uintptr_t> m_output_copy_tracked_resources{};
    std::unordered_set<uintptr_t> m_output_copy_swapchain_buffers{};
    std::atomic<bool> m_output_copy_capture_open{false};
    std::atomic<uint32_t> m_output_copy_boundary_frame{0};
    std::atomic<uint32_t> m_output_copy_boundary_sample{0};
    std::atomic<uint64_t> m_output_copy_event_sequence{0};
    std::atomic<uintptr_t> m_output_copy_color{0};
    uint32_t m_output_copy_last_submit_frame{0};
    uint32_t m_output_copy_submit_ordinal{0};

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
