#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "RE4XeSSFrame.hpp"
#include "RE4XeSSD3D12.hpp"
#include "RE4XeSSRuntime.hpp"

class RE4XeSSWorker final {
public:
    enum class ServiceStatus : uint8_t {
        Ready,
        Waiting,
        Draining,
        Faulted,
        Stale,
        DispatchFailed,
    };

    struct Snapshot {
        bool context_ready{};
        bool execution_ready{};
        bool draining{};
        bool faulted{};
        bool bridge_idle{ true };
        bool bridge_quarantined{};
        bool bridge_device_removed{};
        int32_t mode_token{};
        xess_quality_settings_t quality{ XESS_QUALITY_SETTING_AA };
        xess_2d_t display{};
        RE4XeSSRuntime::InputResolutionQuery input{};
        uintptr_t device_identity{};
        uintptr_t queue_identity{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        std::string failure_reason{};
    };

    struct ControlRequest {
        bool active{};
        int32_t mode_token{};
        std::optional<xess_quality_settings_t> quality{};
        Microsoft::WRL::ComPtr<ID3D12Device> device{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
        xess_2d_t display{};
        std::filesystem::path reframework_directory{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        DWORD caller_thread_id{};
        std::string external_fault{};
    };

    struct ControlResult {
        ServiceStatus status{ ServiceStatus::DispatchFailed };
        bool ready_for_frame{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        Snapshot snapshot{};
    };

    struct SubmitRequest {
        RE4XeSSFrame frame{};
        Microsoft::WRL::ComPtr<ID3D12Resource> color_pin{};
        Microsoft::WRL::ComPtr<ID3D12Resource> depth_pin{};
        Microsoft::WRL::ComPtr<ID3D12Resource> velocity_pin{};
        Microsoft::WRL::ComPtr<ID3D12Resource> output_pin{};
        RE4XeSSD3D12::OutputBinding output{};
        RE4XeSSD3D12::Signature bridge_signature{};
        int32_t mode_token{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        DWORD caller_thread_id{};
    };

    struct SubmitResult {
        enum class Status : uint8_t {
            Submitted,
            Busy,
            Faulted,
            NotReady,
            Stale,
            DispatchFailed,
        };

        Status status{ Status::DispatchFailed };
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        bool bridge_idle{};
        bool bridge_quarantined{};
        bool bridge_device_removed{};
        uint64_t trace_id{};
        uint64_t submit_ordinal{};
        uint64_t writer_fence_value{};
        uint32_t bridge_slot{};
        bool execute_api_succeeded{};
        bool queue_submitted{};
        bool writer_signal_succeeded{};
        Snapshot snapshot{};
        std::string failure_reason{};
    };

    RE4XeSSWorker() = default;
    ~RE4XeSSWorker();

    RE4XeSSWorker(const RE4XeSSWorker&) = delete;
    RE4XeSSWorker& operator=(const RE4XeSSWorker&) = delete;

    bool start(std::string& error);
    ControlResult service_sync(ControlRequest request);
    SubmitResult submit_sync(SubmitRequest request);
    void stop() noexcept;
    DWORD thread_id() const noexcept;

private:
    using Request = std::variant<std::monostate, ControlRequest, SubmitRequest>;
    using Response = std::variant<std::monostate, ControlResult, SubmitResult>;

    struct OwnerConfiguration {
        Microsoft::WRL::ComPtr<ID3D12Device> device{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
        int32_t mode_token{};
        xess_quality_settings_t quality{ XESS_QUALITY_SETTING_AA };
        xess_2d_t display{};
        RE4XeSSRuntime::InputResolutionQuery input{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        bool valid{};
    };

    std::optional<Response> dispatch_sync(Request request);
    void run() noexcept;
    Response process_request(Request request);
    ControlResult process_control(const ControlRequest& request);
    SubmitResult process_submit(SubmitRequest request);
    SubmitResult make_submit_result(
        const SubmitRequest& request,
        SubmitResult::Status status,
        std::string reason = {});
    Snapshot make_snapshot(
        int32_t requested_mode,
        uint64_t control_generation,
        uint64_t device_reset_generation,
        bool draining,
        bool faulted,
        std::string reason = {},
        bool device_removed = false) const;
    void mark_execution_fault(std::string reason, uint64_t control_generation, uint64_t device_reset_generation);
    void shutdown_owned_state() noexcept;
    void quarantine_after_unhandled_exception() noexcept;

    mutable std::mutex m_mutex{};
    std::condition_variable m_request_available{};
    std::condition_variable m_response_ready{};
    std::condition_variable m_startup_ready{};
    std::thread m_thread{};
    Request m_pending_request{};
    Response m_completed_response{};
    uint64_t m_next_sequence{};
    uint64_t m_completed_sequence{};
    bool m_operation_in_flight{};
    bool m_startup_complete{};
    bool m_startup_succeeded{};
    bool m_stop_requested{};
    bool m_worker_exited{};
    std::atomic<DWORD> m_thread_id{};

    std::unique_ptr<RE4XeSSRuntime> m_runtime{};
    std::unique_ptr<RE4XeSSD3D12> m_bridge{};
    OwnerConfiguration m_owner_configuration{};
    bool m_owner_execution_faulted{};
    uint64_t m_owner_fault_control_generation{};
    uint64_t m_owner_fault_device_reset_generation{};
    uint64_t m_owner_device_reset_generation{};
    uint64_t m_last_accepted_control_generation{};
    uint64_t m_last_accepted_device_reset_generation{};
    bool m_owner_waiting_for_new_device{};
    bool m_terminal_faulted{};
    ID3D12Device* m_removed_device_identity{};
    bool m_last_bridge_device_removed{};
    std::string m_owner_failure_reason{};
    uint32_t m_service_log_count{};
    uint32_t m_submit_log_count{};
    std::string m_terminal_failure_reason{};
};
