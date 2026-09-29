#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

class RE4XeSSLifetimeTrace final {
public:
    enum class Kind : uint8_t {
        SceneFrame,
        PreOverlay,
        Submit,
        OutputInstall,
        OutputRestore,
        PostOverlayObservation,
        Present,
        PostPresentCallback,
        Marker,
        WriterFenceComplete,
        DownstreamFenceComplete,
        Skip,
        Count,
    };

    struct Event {
        uint64_t sequence{};
        uint64_t timestamp_us{};
        uint64_t trace_id{};
        uint64_t install_id{};
        uint64_t frame_id{};
        uint64_t scene_ordinal{};
        uint64_t callback_ordinal{};
        uint64_t overlap_epoch{};
        uint64_t submit_ordinal{};
        uint64_t present_ordinal{};
        uint64_t related_present_ordinal{};
        uint64_t output_generation{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        uint64_t writer_sequence{};
        uint64_t writer_invocation_id{};
        uint64_t writer_clear_pre_sequence{};
        uint64_t writer_clear_post_sequence{};
        uint64_t writer_replacement_pre_sequence{};
        uint64_t writer_replacement_post_sequence{};
        uint64_t writer_fence_value{};
        uint64_t downstream_fence_value{};
        uint64_t cached_completed_value{};
        uint64_t actual_completed_value{};
        uint64_t present_entry_time_us{};
        uint64_t original_present_enter_time_us{};
        uint64_t original_present_return_time_us{};
        uintptr_t output_resource{};
        uintptr_t target_state{};
        uintptr_t overlay{};
        uintptr_t swapchain{};
        uintptr_t device{};
        uintptr_t queue{};
        uint32_t thread_id{};
        uint32_t present_entry_thread_id{};
        uint32_t present_return_thread_id{};
        uint32_t bridge_slot{};
        uint32_t writer_method_rva{};
        int32_t result{};
        int32_t present_source{};
        int32_t command_queue_type{ -1 };
        Kind kind{ Kind::Skip };
        bool frame_valid{};
        bool present_valid{};
        bool present_returned{};
        bool present1{};
        bool present_callbacks_suppressed{};
        bool original_present_skipped{};
        bool reset_history{};
        bool api_succeeded{};
        bool queue_submitted{};
        bool writer_signal_succeeded{};
        bool writer_transaction_confirmed{};
        bool successful_submission{};
        bool marker_queued{};
        bool mapping_ambiguous{};
        bool cached_completed_value_valid{};
        bool actual_completed_value_valid{};
        bool command_queue_type_valid{};
        std::array<char, 64> reason{};
    };

    struct Summary {
        std::array<uint64_t, static_cast<size_t>(Kind::Count)> event_counts{};
        uint64_t overwritten_events{};
        uint64_t total_events{};
        uint64_t installed_unmarked{};
        uint64_t installed_unmarked_high_water{};
        uint64_t marked_gpu_incomplete{};
        uint64_t mapping_ambiguities{};
        uint64_t successful_reset_history_submits{};
        uint64_t successful_continuous_submits{};
        uint64_t skipped_submits{};
        uint64_t marker_pending_skips{};
        uint64_t bridge_busy_skips{};
        uint64_t temporal_gate_skips{};
        uint64_t unmatched_markers{};
        uint64_t unmatched_completions{};
        uint64_t evicted_install_records{};
    };

    static constexpr size_t CAPACITY = 4096;

    static RE4XeSSLifetimeTrace& instance() noexcept {
        static RE4XeSSLifetimeTrace trace{};
        return trace;
    }

    void configure(bool enabled) noexcept {
        m_enabled.store(enabled, std::memory_order_release);
    }

    bool enabled() const noexcept {
        return m_enabled.load(std::memory_order_acquire);
    }

    uint64_t next_trace_id() noexcept {
        return m_next_trace_id.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint64_t record(Event event) noexcept {
        if (!enabled()) {
            return 0;
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        event.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        std::lock_guard lock{ m_mutex };
        event.sequence = ++m_sequence;
        m_events[m_next] = event;
        m_next = (m_next + 1) % CAPACITY;
        if (m_size == CAPACITY) {
            ++m_summary.overwritten_events;
        }
        m_size = (std::min)(m_size + 1, CAPACITY);
        ++m_counts[static_cast<size_t>(event.kind)];
        m_summary.event_counts = m_counts;
        ++m_summary.total_events;
        if (event.kind == Kind::PostPresentCallback && event.mapping_ambiguous) {
            ++m_summary.mapping_ambiguities;
        }
        if (event.kind == Kind::OutputInstall) {
            auto& install = m_installs[event.install_id % CAPACITY];
            if (install.active && !install.marked && m_summary.installed_unmarked != 0) {
                --m_summary.installed_unmarked;
                ++m_summary.evicted_install_records;
            } else if (install.active && install.marked && m_summary.marked_gpu_incomplete != 0) {
                --m_summary.marked_gpu_incomplete;
                ++m_summary.evicted_install_records;
            }
            install = InstallState{ event.install_id, 0, true, false };
            ++m_summary.installed_unmarked;
            m_summary.installed_unmarked_high_water = (std::max)(
                m_summary.installed_unmarked_high_water, m_summary.installed_unmarked);
        } else if (event.kind == Kind::Marker && event.marker_queued) {
            auto& install = m_installs[event.install_id % CAPACITY];
            if (install.active && install.install_id == event.install_id && !install.marked) {
                install.marked = true;
                install.fence_value = event.downstream_fence_value;
                if (m_summary.installed_unmarked != 0) {
                    --m_summary.installed_unmarked;
                }
                ++m_summary.marked_gpu_incomplete;
            } else {
                ++m_summary.unmatched_markers;
            }
        } else if (event.kind == Kind::DownstreamFenceComplete && event.marker_queued) {
            auto& install = m_installs[event.install_id % CAPACITY];
            if (install.active && install.install_id == event.install_id && install.marked &&
                event.actual_completed_value >= install.fence_value) {
                --m_summary.marked_gpu_incomplete;
                install = {};
            } else if (event.actual_completed_value_valid) {
                ++m_summary.unmatched_completions;
            }
        } else if (event.kind == Kind::Submit && event.successful_submission) {
            if (event.reset_history) {
                ++m_summary.successful_reset_history_submits;
            } else {
                ++m_summary.successful_continuous_submits;
            }
        } else if (event.kind == Kind::Skip) {
            if (event.frame_valid) {
                ++m_summary.skipped_submits;
                if (std::strcmp(event.reason.data(), "output-handoff-marker-pending") == 0) {
                    ++m_summary.marker_pending_skips;
                } else if (std::strcmp(event.reason.data(), "xess-command-ring-busy") == 0) {
                    ++m_summary.bridge_busy_skips;
                } else if (std::strcmp(event.reason.data(), "pre-overlay-temporal-gate-invalid") == 0) {
                    ++m_summary.temporal_gate_skips;
                }
            }
        }
        return event.sequence;
    }

    std::vector<Event> recent(size_t max_count) const {
        std::lock_guard lock{ m_mutex };
        const auto count = (std::min)(max_count, m_size);
        std::vector<Event> result;
        result.reserve(count);
        const auto first = (m_next + CAPACITY - count) % CAPACITY;
        for (size_t i = 0; i < count; ++i) {
            result.push_back(m_events[(first + i) % CAPACITY]);
        }
        return result;
    }

    std::array<uint64_t, static_cast<size_t>(Kind::Count)> counts() const noexcept {
        std::lock_guard lock{ m_mutex };
        return m_counts;
    }

    Summary summary() const noexcept {
        std::lock_guard lock{ m_mutex };
        return m_summary;
    }

    static void set_reason(Event& event, std::string_view reason) noexcept {
        const auto count = (std::min)(reason.size(), event.reason.size() - 1);
        if (count != 0) {
            std::memcpy(event.reason.data(), reason.data(), count);
        }
        event.reason[count] = '\0';
    }

private:
    struct InstallState {
        uint64_t install_id{};
        uint64_t fence_value{};
        bool active{};
        bool marked{};
    };

    RE4XeSSLifetimeTrace() = default;

    mutable std::mutex m_mutex{};
    std::array<Event, CAPACITY> m_events{};
    std::array<InstallState, CAPACITY> m_installs{};
    std::array<uint64_t, static_cast<size_t>(Kind::Count)> m_counts{};
    Summary m_summary{};
    std::atomic<bool> m_enabled{};
    std::atomic<uint64_t> m_next_trace_id{};
    uint64_t m_sequence{};
    size_t m_next{};
    size_t m_size{};
};
