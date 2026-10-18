#pragma once

#include <cstdint>
#include <atomic>
#include <array>
#include <mutex>
#include <string>
#include <string_view>

#include <d3d12.h>
#include <wrl/client.h>

#include "RE4XeSSD3D12.hpp"
#include "RE4XeSSLifetimeTrace.hpp"
#include "RE4XeSSWriterChain.hpp"
#include <sdk/Renderer.hpp>

class RE4XeSSOutputHandoff final {
public:
    enum class RetirementStatus : uint8_t {
        NoGeneration,
        Active,
        WriterPending,
        MissingMarker,
        Draining,
        Ready,
        Quarantined,
        DeviceRemoved,
    };

    enum class PrepareResult : uint8_t {
        Ready,
        WaitingForPostPresentMarker,
        Failed,
    };

    struct Snapshot {
        bool has_generation{};
        bool installed{};
        RetirementStatus retirement{ RetirementStatus::NoGeneration };
        std::string failure_reason{};
        uint64_t output_generation{};
        uint64_t install_id{};
        uint64_t output_use_token{};
        uint64_t installed_trace_id{};
        uint64_t installed_frame{};
        uint64_t installed_present_ordinal{};
        uint64_t installed_control_generation{};
        uint64_t installed_device_reset_generation{};
        uint64_t last_signaled_fence_value{};
        uint64_t last_completed_fence_value{};
        int32_t installed_mode_token{};
        uintptr_t output_resource{};
        uintptr_t target_state{};
        uintptr_t overlay{};
        bool marker_pending{};
    };

    struct ObservationContext {
        int32_t requested_mode_token{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        uint64_t frame_id{};
        uint32_t callback_thread_id{};
        bool frame_id_valid{};
        uint64_t trace_id{};
        uint64_t callback_ordinal{};
        uint64_t present_ordinal{};
        uint64_t submit_ordinal{};
        uint64_t writer_fence_value{};
        uintptr_t swapchain{};
        uintptr_t device{};
        uintptr_t queue{};
        uint32_t bridge_slot{};
        bool present_ordinal_valid{};
        int32_t command_queue_type{ -1 };
        bool command_queue_type_valid{};
    };

    RE4XeSSOutputHandoff() = default;
    ~RE4XeSSOutputHandoff();

    RE4XeSSOutputHandoff(const RE4XeSSOutputHandoff&) = delete;
    RE4XeSSOutputHandoff& operator=(const RE4XeSSOutputHandoff&) = delete;

    bool restore(
        sdk::renderer::layer::Overlay* layer,
        const ObservationContext& observation,
        const re4_xess::EngineWriterWitnessChain& writer_witness_chain,
        std::string& error,
        uint64_t& consumed_writer_terminal_sequence);
    void request_retirement(std::string_view reason);
    void note_mode_transition(
        uint64_t control_generation,
        int32_t requested_mode_token,
        uint64_t device_reset_generation) noexcept;
    void configure_transition_provenance(
        bool enabled,
        std::string_view reason,
        uintptr_t image_base,
        uint32_t image_size,
        uint32_t image_checksum) noexcept;
    RetirementStatus poll_retirement(bool bridge_writer_idle, bool bridge_device_removed);
    PrepareResult prepare(
        sdk::renderer::layer::Overlay* layer,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12Resource* semantic_color,
        uint32_t render_width,
        uint32_t render_height,
        uint32_t display_width,
        uint32_t display_height,
        uint64_t control_generation,
        bool bridge_writer_idle,
        bool bridge_device_removed,
        RE4XeSSD3D12::OutputBinding& output,
        std::string& error);
    void note_submission_succeeded();
    bool install(
        sdk::renderer::layer::Overlay* layer,
        const ObservationContext& observation,
        std::string& error);
    void quarantine(std::string_view reason, bool bridge_writer_uncertain = false) noexcept;
    void on_post_present(
        ID3D12Device* active_device,
        ID3D12CommandQueue* active_queue,
        const ObservationContext& observation) noexcept;
    void observe_fence_completion() noexcept;
    void observe_overlay(
        sdk::renderer::layer::Overlay* layer,
        const ObservationContext& observation,
        const re4_xess::EngineWriterWitnessChain& writer_witness_chain);
    bool identity_mismatch_latched() const noexcept;
    Snapshot snapshot() const;

private:
    struct Signature {
        uintptr_t template_state{};
        uintptr_t template_resource{};
        uintptr_t device{};
        uintptr_t queue{};
        uint64_t control_generation{};
        uint32_t display_width{};
        uint32_t display_height{};
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };

        bool operator==(const Signature&) const = default;
    };

    bool validate_and_clone(
        sdk::renderer::layer::Overlay* layer,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12Resource* semantic_color,
        uint32_t render_width,
        uint32_t render_height,
        uint32_t display_width,
        uint32_t display_height,
        std::string& error);
    void release_generation() noexcept;
    void preserve_quarantined_generation() noexcept;
    bool confirmed_device_removal(bool bridge_device_removed) const noexcept;
    bool is_verified_engine_replacement_locked(
        const re4_xess::EngineWriterWitnessChain& witness_chain,
        uintptr_t layer_identity,
        uintptr_t slot_identity,
        uintptr_t observed_state,
        const ObservationContext& observation,
        re4_xess::EngineWriterChainValidation* validation = nullptr) const noexcept;
    bool consume_mode_transition_observation(const ObservationContext& observation) noexcept;
    void log_transition_provenance(
        std::string_view phase,
        const ObservationContext& observation,
        uintptr_t layer_identity,
        uintptr_t current_state_identity,
        bool current_state_known,
        uint32_t phase_bit) noexcept;
    uint64_t find_matching_provenance_divergence(
        uintptr_t layer_identity,
        uintptr_t slot_identity,
        uintptr_t current_state_identity,
        uintptr_t expected_state_identity,
        uint64_t control_generation,
        uint64_t device_reset_generation) noexcept;

    struct ProvenanceSample {
        std::string_view phase{};
        uint64_t sequence{};
        int64_t timestamp_us{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        uint64_t frame_id{};
        uint32_t thread_id{};
        int32_t requested_mode_token{};
        int32_t installed_mode_token{};
        uintptr_t overlay{};
        uintptr_t slot{};
        uintptr_t current_state{};
        uintptr_t expected_state{};
        uintptr_t saved_state{};
        bool frame_id_valid{};
        bool slot_read_valid{};
        bool first_divergence{};
        bool identity_changed{};
        bool marker_pending{};
        bool installed{};
        bool retirement_requested{};
        bool hard_quarantined{};
    };

    sdk::intrusive_ptr<sdk::renderer::TargetState> m_handoff_state{};
    sdk::intrusive_ptr<sdk::renderer::TargetState> m_saved_original_state{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource_pin{};
    Microsoft::WRL::ComPtr<ID3D12Device> m_device{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Fence> m_retirement_fence{};
    sdk::renderer::layer::Overlay* m_installed_overlay{};
    Signature m_signature{};
    D3D12_RESOURCE_STATES m_expected_state{ D3D12_RESOURCE_STATE_COMMON };
    uint64_t m_next_retirement_value{ 1 };
    uint64_t m_last_signaled_retirement_value{};
    uint64_t m_last_completed_retirement_value{};
    uint64_t m_installed_frame{};
    uint64_t m_next_output_generation{};
    uint64_t m_output_generation_id{};
    uint64_t m_next_install_id{};
    uint64_t m_installed_install_id{};
    RE4XeSSLifetimeTrace::OutputUseTokenIssuer m_output_use_token_issuer{};
    uint64_t m_installed_trace_id{};
    uint64_t m_installed_present_ordinal{};
    uint64_t m_installed_submit_ordinal{};
    uint64_t m_installed_writer_fence_value{};
    uint32_t m_installed_bridge_slot{};
    uint64_t m_trace_last_observed_completed_value{};
    struct TraceMarker {
        uint64_t trace_id{};
        uint64_t install_id{};
        uint64_t frame_id{};
        uint64_t present_ordinal{};
        uint64_t output_generation{};
        uint64_t fence_value{};
        uintptr_t output_resource{};
        uint32_t thread_id{};
    };
    std::array<TraceMarker, 128> m_trace_markers{};
    size_t m_trace_marker_count{};
    uint64_t m_installed_device_reset_generation{};
    int32_t m_installed_mode_token{};
    uintptr_t m_last_confirmed_post_overlay_layer{};
    uintptr_t m_last_confirmed_post_overlay_state{};
    uint64_t m_last_confirmed_post_overlay_frame{};
    mutable uint32_t m_retirement_log_count{};
    uint32_t m_restore_log_count{};
    uint32_t m_delayed_marker_log_count{};
    bool m_installed{};
    bool m_retirement_requested{};
    bool m_downstream_use_seen{};
    bool m_marker_pending{};
    bool m_missing_marker{};
    bool m_marker_wait_logged{};
    bool m_marker_recovery_pending{};
    bool m_hard_quarantined{};
    bool m_bridge_writer_uncertain{};
    bool m_missing_marker_logged{};
    bool m_quarantine_logged{};
    bool m_device_removed{};
    bool m_post_overlay_mismatch_logged{};
    bool m_last_confirmed_post_overlay_valid{};
    uint64_t m_last_consumed_engine_writer_sequence{};
    uint64_t m_engine_replacement_observed_sequence{};
    std::atomic<bool> m_identity_mismatch_latched{};
    std::atomic<uint64_t> m_mode_transition_generation{};
    std::atomic<uint32_t> m_mode_transition_observation_budget{};
    std::atomic<uint64_t> m_provenance_generation{};
    std::atomic<uint32_t> m_provenance_phase_mask{};
    std::atomic<uint32_t> m_provenance_event_budget{};
    std::atomic<uint32_t> m_provenance_window_budget{};
    std::atomic<uintptr_t> m_provenance_last_current_state{};
    std::atomic<uintptr_t> m_provenance_validated_layer{};
    std::atomic<bool> m_provenance_enabled{};
    std::array<ProvenanceSample, 16> m_provenance_ring{};
    size_t m_provenance_ring_next{};
    size_t m_provenance_ring_size{};
    uint64_t m_provenance_event_count{};
    uint64_t m_provenance_divergence_count{};
    uint64_t m_provenance_invalid_slot_count{};
    std::mutex m_provenance_mutex{};
    std::atomic<bool> m_has_generation_snapshot{};
    std::atomic<bool> m_installed_snapshot{};
    RetirementStatus m_retirement_status{ RetirementStatus::NoGeneration };
    mutable std::mutex m_retirement_mutex{};
    std::string m_failure_reason{};
};
