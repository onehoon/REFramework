#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include <Windows.h>

class XeFGResizeLifecycle {
public:
    enum class EventKind : uint8_t { None, ResizeTarget, ResizeBuffers, ResizeBuffers1 };

    struct PostResizePresentSample {
        uint64_t event_id{};
        EventKind kind{EventKind::None};
        uint32_t ordinal{};
        std::chrono::milliseconds elapsed{};
    };

    uint64_t begin(EventKind kind);
    bool arm(uint64_t trigger_event_id);
    bool complete(uint64_t completion_event_id, EventKind completion_kind, HRESULT result);
    bool clear();

    bool suppress_renderer() const noexcept { return m_hold_active; }
    uint64_t event_id() const noexcept { return m_event_id; }
    EventKind last_kind() const noexcept { return m_last_kind; }
    uint64_t hold_trigger_event_id() const noexcept { return m_hold_trigger_event_id; }
    uint32_t note_suppressed_present() noexcept { return ++m_suppressed_present_count; }
    uint32_t suppressed_present_count() const noexcept { return m_suppressed_present_count; }
    std::optional<PostResizePresentSample> consume_post_resize_present_sample();

private:
    uint64_t m_event_id{};
    EventKind m_last_kind{EventKind::None};
    std::chrono::steady_clock::time_point m_last_event_time{};
    bool m_hold_active{};
    uint64_t m_hold_trigger_event_id{};
    uint32_t m_suppressed_present_count{};
    uint32_t m_post_resize_present_budget{};
    uint32_t m_post_resize_present_ordinal{};
};

