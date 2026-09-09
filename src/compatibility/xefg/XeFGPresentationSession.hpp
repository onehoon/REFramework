#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "XeFGBinding.hpp"
#include "XeFGResizeLifecycle.hpp"

struct XeFGMonitorBindingKey {
    uint64_t generation{};
    size_t runtime_slot{XeFGBinding::kInvalidRuntimeSlot};
    void* runtime_context{};
    IDXGISwapChain3* swapchain{};
    void* hook_target{};

    bool operator==(const XeFGMonitorBindingKey& other) const noexcept {
        return generation == other.generation
            && runtime_slot == other.runtime_slot
            && runtime_context == other.runtime_context
            && swapchain == other.swapchain
            && hook_target == other.hook_target;
    }
};

class XeFGHookMonitorState {
public:
    enum class TimeoutClass : uint8_t {
        Grace,
        Sustained,
    };

    TimeoutClass note_timeout(
        const XeFGMonitorBindingKey& key,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    void clear() noexcept;

    uint32_t consecutive_timeouts() const noexcept {
        return m_consecutive_timeouts;
    }

private:
    static constexpr uint32_t kSustainedTimeoutThreshold = 3;
    static constexpr int64_t kMinimumSustainedPresentAgeMs = 20000;

    XeFGMonitorBindingKey m_key{};
    uint64_t m_last_present_entry_count{};
    uint32_t m_consecutive_timeouts{};
    bool m_initialized{};
};

struct XeFGDetachedState {
    bool active{};
    XeFGBinding::RuntimeIdentity previous_runtime{};
    uint64_t previous_generation{};
    const char* reason{};
};

class XeFGPresentationSession {
public:
    enum class MonitorDisposition : uint8_t {
        AllowGenericRecovery,
        PreserveGrace,
        SuppressRuntimeTransition,
        SuppressDetachedUncertain,
        QuarantineSustainedTimeout,
        QuarantineInconsistentState,
    };

    struct MonitorEvaluation {
        MonitorDisposition disposition{MonitorDisposition::AllowGenericRecovery};
        const char* reason{"xefg_state_safe"};
        XeFGBinding::RuntimeLifecycleSnapshot binding{};
        XeFGMonitorBindingKey key{};
        uint64_t present_entry_count{};
        int64_t present_age_ms{-1};
        uint32_t timeout_count{};
        bool detached_uncertain{};
        bool action_changed{};
    };

    struct PresentDecision {
        bool xefg_source{};
        bool resize_hold_active{};
        bool suppress_render_callbacks{};
        bool log_first_render_boundary{};
        uint64_t resize_event_id{};
        uint64_t hold_trigger_event_id{};
    };

    enum class RuntimeDetachMatch : uint8_t {
        None,
        ExactRuntime,
        SameHwnd,
    };

    struct RuntimeDetachEvaluation {
        bool accepted{};
        RuntimeDetachMatch match{RuntimeDetachMatch::None};
        XeFGBinding::RuntimeLifecycleSnapshot binding{};
    };

    struct DestroyReconciliation {
        bool accepted{};
        uint64_t previous_generation{};
    };

    struct PhysicalBindingView {
        bool hook_active{};
        bool phase1{};
        bool xefg_source{};
        bool swapchain_hook_present{};
        IDXGISwapChain3* renderer_swapchain{};
        ID3D12CommandQueue* renderer_queue{};
        ID3D12Device4* renderer_device{};
        void* hook_target{};
    };

    XeFGPresentationSession() = default;

    XeFGBinding& binding() noexcept { return m_binding; }
    const XeFGBinding& binding() const noexcept { return m_binding; }

    XeFGResizeLifecycle& resize_lifecycle() noexcept { return m_resize_lifecycle; }
    const XeFGResizeLifecycle& resize_lifecycle() const noexcept { return m_resize_lifecycle; }

    XeFGDetachedState& detached_state() noexcept { return m_detached_state; }
    const XeFGDetachedState& detached_state() const noexcept { return m_detached_state; }

    bool has_monitor_state(bool xefg_source) const noexcept;
    bool detached_uncertain() const noexcept { return m_detached_state.active; }
    bool consistent_with(const PhysicalBindingView& physical) const noexcept;
    XeFGMonitorBindingKey monitor_binding_key(void* hook_target) const noexcept;
    XeFGHookMonitorState::TimeoutClass note_monitor_timeout(
        void* hook_target,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;
    uint32_t monitor_timeout_count() const noexcept {
        return m_monitor_state.consecutive_timeouts();
    }
    bool note_monitor_action(const char* action) noexcept;
    void clear_monitor_state() noexcept;
    MonitorEvaluation evaluate_monitor_timeout(
        const PhysicalBindingView& physical,
        bool runtime_transition_active,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    RuntimeDetachEvaluation evaluate_runtime_detach(
        size_t runtime_slot,
        void* context,
        HWND hwnd,
        bool allow_same_hwnd_match,
        bool xefg_source) const noexcept;
    void begin_runtime_detach(const RuntimeDetachEvaluation& evaluation, const char* reason) noexcept;
    void complete_runtime_detach() noexcept;
    DestroyReconciliation evaluate_destroy_result(size_t runtime_slot, void* context, int32_t result) const noexcept;
    void commit_destroy_reconciliation(const DestroyReconciliation& reconciliation) noexcept;

    PresentDecision evaluate_present_policy(
        bool xefg_source,
        bool render_callback_available) const noexcept;
    uint32_t note_suppressed_present(const PresentDecision& decision) noexcept;
    void mark_render_boundary_logged(const PresentDecision& decision) noexcept;

    bool render_boundary_logged() const noexcept { return m_render_boundary_logged; }
    void set_render_boundary_logged(bool value) noexcept { m_render_boundary_logged = value; }

private:
    XeFGBinding m_binding{};
    XeFGResizeLifecycle m_resize_lifecycle{};
    XeFGDetachedState m_detached_state{};
    XeFGHookMonitorState m_monitor_state{};
    const char* m_last_monitor_action{};
    bool m_render_boundary_logged{};
};
