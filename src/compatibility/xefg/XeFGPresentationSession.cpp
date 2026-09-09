#include "XeFGPresentationSession.hpp"

#include "XeFGResult.hpp"

bool XeFGPresentationSession::has_monitor_state(bool xefg_source) const noexcept {
    return m_detached_state.active
        || m_binding.active()
        || xefg_source;
}

bool XeFGPresentationSession::consistent_with(const PhysicalBindingView& physical) const noexcept {
    return physical.hook_active
        && !physical.phase1
        && physical.xefg_source
        && m_binding.active()
        && physical.swapchain_hook_present
        && m_binding.aliases_match(physical.renderer_swapchain, physical.renderer_queue, physical.renderer_device)
        && physical.hook_target == static_cast<void*>(m_binding.swapchain());
}

XeFGMonitorBindingKey XeFGPresentationSession::monitor_binding_key(void* hook_target) const noexcept {
    const auto snapshot = m_binding.lifecycle_snapshot();
    return {
        snapshot.generation,
        snapshot.runtime.slot,
        snapshot.runtime.context,
        snapshot.swapchain,
        hook_target,
    };
}

XeFGHookMonitorState::TimeoutClass XeFGPresentationSession::note_monitor_timeout(
    void* hook_target,
    uint64_t present_entry_count,
    int64_t present_age_ms) noexcept {
    return m_monitor_state.note_timeout(
        monitor_binding_key(hook_target),
        present_entry_count,
        present_age_ms);
}

bool XeFGPresentationSession::note_monitor_action(const char* action) noexcept {
    if (m_last_monitor_action == action) {
        return false;
    }

    m_last_monitor_action = action;
    return true;
}

void XeFGPresentationSession::clear_monitor_state() noexcept {
    m_monitor_state.clear();
    m_last_monitor_action = nullptr;
}

XeFGPresentationSession::MonitorEvaluation XeFGPresentationSession::evaluate_monitor_timeout(
    const PhysicalBindingView& physical,
    bool runtime_transition_active,
    uint64_t present_entry_count,
    int64_t present_age_ms) noexcept {
    MonitorEvaluation result{};
    result.present_entry_count = present_entry_count;
    result.present_age_ms = present_age_ms;

    if (runtime_transition_active) {
        result.disposition = MonitorDisposition::SuppressRuntimeTransition;
        result.reason = "runtime_transition";
    } else if (!has_monitor_state(physical.xefg_source)) {
        result.disposition = MonitorDisposition::AllowGenericRecovery;
        result.reason = "xefg_state_safe";
    } else if (detached_uncertain()) {
        result.disposition = MonitorDisposition::SuppressDetachedUncertain;
        result.reason = "detached_uncertain";
    } else if (!consistent_with(physical)) {
        result.disposition = MonitorDisposition::QuarantineInconsistentState;
        result.reason = "binding_identity_inconsistent";
    } else if (note_monitor_timeout(physical.hook_target, present_entry_count, present_age_ms)
        == XeFGHookMonitorState::TimeoutClass::Sustained) {
        result.disposition = MonitorDisposition::QuarantineSustainedTimeout;
        result.reason = "sustained_present_timeout";
    } else {
        result.disposition = MonitorDisposition::PreserveGrace;
        result.reason = "present_timeout";
    }

    result.action_changed = note_monitor_action(result.reason);
    result.binding = m_binding.lifecycle_snapshot();
    result.key = monitor_binding_key(physical.hook_target);
    result.timeout_count = monitor_timeout_count();
    result.detached_uncertain = detached_uncertain();
    return result;
}

XeFGPresentationSession::RuntimeDetachEvaluation XeFGPresentationSession::evaluate_runtime_detach(
    size_t runtime_slot,
    void* context,
    HWND hwnd,
    bool allow_same_hwnd_match,
    bool xefg_source) const noexcept {
    const auto snapshot = m_binding.lifecycle_snapshot();
    const bool exact_runtime_match = snapshot.active
        && context != nullptr
        && snapshot.runtime.slot == runtime_slot
        && snapshot.runtime.context == context;
    const bool same_hwnd_match = allow_same_hwnd_match
        && hwnd != nullptr
        && snapshot.active
        && snapshot.runtime.hwnd == hwnd;
    const auto match = exact_runtime_match
        ? RuntimeDetachMatch::ExactRuntime
        : (same_hwnd_match ? RuntimeDetachMatch::SameHwnd : RuntimeDetachMatch::None);

    return {
        (exact_runtime_match || same_hwnd_match) && xefg_source && snapshot.active,
        match,
        snapshot,
    };
}

void XeFGPresentationSession::begin_runtime_detach(
    const RuntimeDetachEvaluation& evaluation,
    const char* reason) noexcept {
    if (!evaluation.accepted) {
        return;
    }

    m_detached_state = {
        true,
        evaluation.binding.runtime,
        evaluation.binding.generation,
        reason != nullptr ? reason : "unknown",
    };
    clear_monitor_state();
}

void XeFGPresentationSession::complete_runtime_detach() noexcept {
    m_binding.clear();
}

XeFGPresentationSession::DestroyReconciliation XeFGPresentationSession::evaluate_destroy_result(
    size_t runtime_slot,
    void* context,
    int32_t result) const noexcept {
    return {
        m_detached_state.active
            && xefg_result::succeeded(result)
            && m_detached_state.previous_runtime.slot == runtime_slot
            && m_detached_state.previous_runtime.context == context,
        m_detached_state.previous_generation,
    };
}

void XeFGPresentationSession::commit_destroy_reconciliation(
    const DestroyReconciliation& reconciliation) noexcept {
    if (!reconciliation.accepted) {
        return;
    }

    m_detached_state = {};
    clear_monitor_state();
}

XeFGHookMonitorState::TimeoutClass XeFGHookMonitorState::note_timeout(
    const XeFGMonitorBindingKey& key,
    uint64_t present_entry_count,
    int64_t present_age_ms) noexcept {
    if (!m_initialized || !(m_key == key) || m_last_present_entry_count != present_entry_count) {
        m_key = key;
        m_last_present_entry_count = present_entry_count;
        m_consecutive_timeouts = 1;
        m_initialized = true;
        return TimeoutClass::Grace;
    }

    ++m_consecutive_timeouts;
    return m_consecutive_timeouts >= kSustainedTimeoutThreshold
        && present_age_ms >= kMinimumSustainedPresentAgeMs
        ? TimeoutClass::Sustained
        : TimeoutClass::Grace;
}

void XeFGHookMonitorState::clear() noexcept {
    m_key = {};
    m_last_present_entry_count = 0;
    m_consecutive_timeouts = 0;
    m_initialized = false;
}
