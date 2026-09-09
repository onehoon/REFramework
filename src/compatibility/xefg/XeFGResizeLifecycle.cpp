#include "XeFGResizeLifecycle.hpp"

uint64_t XeFGResizeLifecycle::begin(EventKind kind) {
    ++m_event_id;
    m_last_kind = kind;
    m_last_event_time = std::chrono::steady_clock::now();
    m_post_resize_present_budget = 3;
    m_post_resize_present_ordinal = 0;
    return m_event_id;
}

bool XeFGResizeLifecycle::arm(uint64_t trigger_event_id) {
    if (trigger_event_id == 0) {
        return false;
    }
    m_hold_active = true;
    m_hold_trigger_event_id = trigger_event_id;
    m_suppressed_present_count = 0;
    return true;
}

bool XeFGResizeLifecycle::complete(uint64_t, EventKind, HRESULT result) {
    if (!m_hold_active || FAILED(result)) {
        return false;
    }
    m_hold_active = false;
    m_hold_trigger_event_id = 0;
    m_suppressed_present_count = 0;
    return true;
}

bool XeFGResizeLifecycle::clear() {
    if (!m_hold_active) {
        return false;
    }
    m_hold_active = false;
    m_hold_trigger_event_id = 0;
    m_suppressed_present_count = 0;
    return true;
}

std::optional<XeFGResizeLifecycle::PostResizePresentSample> XeFGResizeLifecycle::consume_post_resize_present_sample() {
    if (m_post_resize_present_budget == 0) {
        return std::nullopt;
    }
    --m_post_resize_present_budget;
    PostResizePresentSample sample{};
    sample.event_id = m_event_id;
    sample.kind = m_last_kind;
    sample.ordinal = ++m_post_resize_present_ordinal;
    sample.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_last_event_time);
    return sample;
}
