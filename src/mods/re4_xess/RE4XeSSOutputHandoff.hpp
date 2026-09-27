#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include <d3d12.h>
#include <wrl/client.h>

#include "RE4XeSSD3D12.hpp"
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

    struct Snapshot {
        bool has_generation{};
        bool installed{};
        RetirementStatus retirement{ RetirementStatus::NoGeneration };
        std::string failure_reason{};
    };

    RE4XeSSOutputHandoff() = default;
    ~RE4XeSSOutputHandoff();

    RE4XeSSOutputHandoff(const RE4XeSSOutputHandoff&) = delete;
    RE4XeSSOutputHandoff& operator=(const RE4XeSSOutputHandoff&) = delete;

    bool restore(sdk::renderer::layer::Overlay* layer, std::string& error);
    void request_retirement(std::string_view reason);
    RetirementStatus poll_retirement(bool bridge_writer_idle, bool bridge_device_removed);
    bool prepare(
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
    bool install(sdk::renderer::layer::Overlay* layer, uint64_t frame_id, std::string& error);
    void quarantine(std::string_view reason, bool bridge_writer_uncertain = false) noexcept;
    void on_post_present(ID3D12Device* active_device, ID3D12CommandQueue* active_queue) noexcept;
    void observe_overlay(sdk::renderer::layer::Overlay* layer) const;
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
    std::atomic<bool> m_has_generation_snapshot{};
    std::atomic<bool> m_installed_snapshot{};
    RetirementStatus m_retirement_status{ RetirementStatus::NoGeneration };
    mutable std::mutex m_retirement_mutex{};
    std::string m_failure_reason{};
};
