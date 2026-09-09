#include "XeFGPresentationSession.hpp"

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
