#include "XeFGPresentationSession.hpp"

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
