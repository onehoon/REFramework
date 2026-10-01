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
        SceneViewSize,
        PreOverlay,
        Submit,
        OutputInstall,
        OverlayRenderContext,
        OutputRestore,
        ModeTransition,
        PostOverlayObservation,
        Present,
        PostPresentCallback,
        Marker,
        WriterFenceComplete,
        DownstreamFenceComplete,
        Skip,
        Count,
    };

    enum class CapturePhase : uint8_t {
        PreActive,
        ActiveXeSS,
    };

    enum class MappingState : uint8_t {
        Unknown,
        InferredCandidate,
        Proven,
    };

    enum class RenderContextStage : uint8_t {
        None,
        BeforeOriginalOverlayDraw,
        PostOverlayCallbackOriginalStatusUnknown,
    };

    enum class ConsumerEvidence : uint8_t {
        Unknown,
        ReaderNotObserved,
        ReaderObservedSubmitUnproven,
        QueueOrderProven,
        PresentConsumerProven,
        GpuComplete,
    };

    class OutputUseTokenIssuer final {
    public:
        // This diagnostic identity is independent of install_id and never authorizes resource reuse.
        uint64_t on_install_result(bool install_succeeded, bool trace_enabled) noexcept {
            if (!install_succeeded) {
                return 0;
            }

            if (!trace_enabled) {
                m_active_token = 0;
                return 0;
            }

            if (++m_next_token == 0) {
                ++m_next_token;
            }
            m_active_token = m_next_token;
            return m_active_token;
        }

        void retire_generation() noexcept {
            m_active_token = 0;
        }

        uint64_t active_token() const noexcept {
            return m_active_token;
        }

    private:
        uint64_t m_next_token{};
        uint64_t m_active_token{};
    };

    enum class DumpWindow : uint8_t {
        PreActive,
        ActiveMilestone,
        Anomaly,
        ModeTransition,
        Periodic,
        Count,
    };

    enum class AnomalyWindow : uint8_t {
        FirstMarkerPending,
        MarkerPendingAcrossPresent,
        FirstUnprovenPresentMapping,
        WriterOrFenceFailure,
        Count,
    };

    struct DumpReservation {
        DumpWindow window{ DumpWindow::PreActive };
        uint32_t index{};
        size_t event_count{};
        size_t estimated_bytes{};
        bool active_phase{};
    };

    struct Event {
        uint64_t sequence{};
        uint64_t timestamp_us{};
        uint64_t trace_id{};
        uint64_t install_id{};
        uint64_t output_use_token{}; // Issued per install; only an observed reader may propagate it.
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
        uint32_t input_width{};
        uint32_t input_height{};
        uint32_t display_width{};
        uint32_t display_height{};
        uint32_t jitter_sample_index{};
        uint32_t jitter_phase_count{};
        float scene_view_native_width{};
        float scene_view_native_height{};
        float scene_view_effective_width{};
        float scene_view_effective_height{};
        float jitter_x_pixels{};
        float jitter_y_pixels{};
        uintptr_t output_resource{};
        uintptr_t target_state{};
        uintptr_t scene_view{};
        uintptr_t render_context{};
        uintptr_t render_context_target_state{};
        uintptr_t render_context_target_resource{};
        uintptr_t overlay_main_target_state{};
        uintptr_t overlay_main_target_resource{};
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
        bool render_context_sample_valid{};
        bool render_context_target_state_matches_output{};
        bool render_context_target_resource_matches_output{};
        bool render_context_target_state_matches_overlay_main{};
        bool render_context_target_resource_matches_overlay_main{};
        bool cached_completed_value_valid{};
        bool actual_completed_value_valid{};
        bool command_queue_type_valid{};
        bool mapping_observed{};
        bool input_resolution_valid{};
        bool temporal_active{};
        bool scene_view_override_applied{};
        bool scene_view_conflict{};
        bool jitter_applied{};
        CapturePhase capture_phase{ CapturePhase::PreActive };
        MappingState mapping_state{ MappingState::Unknown };
        RenderContextStage render_context_stage{ RenderContextStage::None };
        ConsumerEvidence consumer_evidence{ ConsumerEvidence::Unknown };
        std::array<char, 64> reason{};
    };

    struct Summary {
        std::array<uint64_t, static_cast<size_t>(Kind::Count)> event_counts{};
        std::array<uint64_t, static_cast<size_t>(Kind::Count)> pre_active_event_counts{};
        std::array<uint64_t, static_cast<size_t>(Kind::Count)> active_event_counts{};
        uint64_t overwritten_events{};
        uint64_t total_events{};
        uint64_t pre_active_events{};
        uint64_t active_events{};
        uint64_t pre_active_overwritten_events{};
        uint64_t active_overwritten_events{};
        uint64_t active_successful_submissions{};
        uint64_t active_reset_history_submits{};
        uint64_t active_continuous_submits{};
        uint64_t active_skipped_submits{};
        uint64_t active_marker_queued{};
        uint64_t active_output_installs{};
        uint64_t active_marker_pending_skips{};
        uint64_t active_render_context_samples{};
        uint64_t active_render_context_state_matches{};
        uint64_t active_render_context_resource_matches{};
        uint64_t active_render_context_overlay_main_state_matches{};
        uint64_t active_render_context_overlay_main_resource_matches{};
        uint64_t active_bridge_busy_skips{};
        uint64_t active_temporal_gate_skips{};
        uint64_t scene_view_size_conflicts{};
        uint64_t active_scene_view_size_conflicts{};
        uint64_t scene_view_size_observation_drops{};
        uint64_t active_scene_view_size_observation_drops{};
        uint64_t scene_view_frame_cache_evictions{};
        uint64_t active_scene_view_frame_cache_evictions{};
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
        uint64_t mapping_unknown{};
        uint64_t mapping_inferred_candidates{};
        uint64_t mapping_proven{};
        uint64_t active_mapping_unknown{};
        uint64_t active_mapping_inferred_candidates{};
        uint64_t active_mapping_proven{};
        uint64_t dump_requests{};
        uint64_t dump_windows_emitted{};
        uint64_t dump_windows_suppressed{};
        uint64_t dump_event_lines_reserved{};
        uint64_t dump_event_lines_emitted{};
        uint64_t dump_bytes_reserved{};
        uint64_t active_gameplay_dump_emitted{};
        uint64_t retained_events{};
        bool active_capture_started{};
    };

    static constexpr size_t CAPACITY = 4096;
    static constexpr size_t SCENE_VIEW_FRAME_CACHE_CAPACITY = 8;
    static constexpr size_t MAX_SCENE_VIEW_OBSERVATIONS_PER_FRAME = 8;
    static constexpr size_t MAX_EVENTS_PER_DUMP = 32;
    static constexpr size_t MAX_DUMP_EVENT_LINES = 512;
    static constexpr size_t MAX_DUMP_BYTES = 1'100'000;

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
        if (event.kind == Kind::SceneViewSize && event.frame_valid &&
            !admit_scene_view_observation_locked(event)) {
            return 0;
        }
        if (event.kind == Kind::OutputInstall) {
            const bool starting_active_capture = !m_active_capture_started;
            m_active_capture_started = true;
            m_active_capture_started_snapshot.store(true, std::memory_order_release);
            if (starting_active_capture && event.trace_id != 0) {
                // The first successful Execute immediately precedes its OutputInstall in
                // the same coordinator transaction; count that submit in the active window.
                const auto preceding_count = (std::min)(m_size, MAX_EVENTS_PER_DUMP);
                for (size_t offset = 0; offset < preceding_count; ++offset) {
                    const auto event_index = (m_next + CAPACITY - 1 - offset) % CAPACITY;
                    auto& prior = m_events[event_index];
                    if (prior.trace_id == event.trace_id && prior.kind == Kind::Submit &&
                        prior.successful_submission && prior.capture_phase == CapturePhase::PreActive) {
                        prior.capture_phase = CapturePhase::ActiveXeSS;
                        --m_summary.pre_active_events;
                        --m_summary.pre_active_event_counts[static_cast<size_t>(Kind::Submit)];
                        ++m_summary.active_events;
                        ++m_summary.active_event_counts[static_cast<size_t>(Kind::Submit)];
                        ++m_summary.active_successful_submissions;
                        if (prior.reset_history) {
                            ++m_summary.active_reset_history_submits;
                        } else {
                            ++m_summary.active_continuous_submits;
                        }
                        break;
                    }
                }
            }
        }
        event.capture_phase = m_active_capture_started
            ? CapturePhase::ActiveXeSS
            : CapturePhase::PreActive;
        event.sequence = ++m_sequence;
        if (m_size == CAPACITY) {
            const auto& overwritten = m_events[m_next];
            if (overwritten.capture_phase == CapturePhase::ActiveXeSS) {
                ++m_summary.active_overwritten_events;
            } else {
                ++m_summary.pre_active_overwritten_events;
            }
        }
        m_events[m_next] = event;
        m_next = (m_next + 1) % CAPACITY;
        if (m_size == CAPACITY) {
            ++m_summary.overwritten_events;
        }
        m_size = (std::min)(m_size + 1, CAPACITY);
        ++m_counts[static_cast<size_t>(event.kind)];
        m_summary.event_counts = m_counts;
        ++m_summary.total_events;
        if (event.capture_phase == CapturePhase::ActiveXeSS) {
            ++m_summary.active_events;
            ++m_summary.active_event_counts[static_cast<size_t>(event.kind)];
        } else {
            ++m_summary.pre_active_events;
            ++m_summary.pre_active_event_counts[static_cast<size_t>(event.kind)];
        }
        if (event.capture_phase == CapturePhase::ActiveXeSS && event.render_context_sample_valid) {
            ++m_summary.active_render_context_samples;
            if (event.render_context_target_state_matches_output) {
                ++m_summary.active_render_context_state_matches;
            }
            if (event.render_context_target_resource_matches_output) {
                ++m_summary.active_render_context_resource_matches;
            }
            if (event.render_context_target_state_matches_overlay_main) {
                ++m_summary.active_render_context_overlay_main_state_matches;
            }
            if (event.render_context_target_resource_matches_overlay_main) {
                ++m_summary.active_render_context_overlay_main_resource_matches;
            }
        }
        if (event.mapping_observed) {
            switch (event.mapping_state) {
            case MappingState::Unknown:
                ++m_summary.mapping_unknown;
                if (event.capture_phase == CapturePhase::ActiveXeSS) ++m_summary.active_mapping_unknown;
                break;
            case MappingState::InferredCandidate:
                ++m_summary.mapping_inferred_candidates;
                if (event.capture_phase == CapturePhase::ActiveXeSS) ++m_summary.active_mapping_inferred_candidates;
                break;
            case MappingState::Proven:
                ++m_summary.mapping_proven;
                if (event.capture_phase == CapturePhase::ActiveXeSS) ++m_summary.active_mapping_proven;
                break;
            }
        }
        m_summary.retained_events = m_size;
        m_summary.active_capture_started = m_active_capture_started;
        if (event.kind == Kind::PostPresentCallback && event.mapping_ambiguous) {
            ++m_summary.mapping_ambiguities;
        }
        if (event.kind == Kind::OutputInstall) {
            ++m_summary.active_output_installs;
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
            if (event.capture_phase == CapturePhase::ActiveXeSS) {
                ++m_summary.active_marker_queued;
            }
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
            m_first_execute_success_seen = true;
            if (event.capture_phase == CapturePhase::ActiveXeSS) {
                ++m_summary.active_successful_submissions;
            }
            if (event.reset_history) {
                ++m_summary.successful_reset_history_submits;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_reset_history_submits;
                }
            } else {
                ++m_summary.successful_continuous_submits;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_continuous_submits;
                }
            }
        } else if (event.kind == Kind::Skip) {
            if (event.frame_valid) {
                ++m_summary.skipped_submits;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_skipped_submits;
                }
            }
            if (std::strcmp(event.reason.data(), "output-handoff-marker-pending") == 0) {
                ++m_summary.marker_pending_skips;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_marker_pending_skips;
                }
            } else if (std::strcmp(event.reason.data(), "xess-command-ring-busy") == 0) {
                ++m_summary.bridge_busy_skips;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_bridge_busy_skips;
                }
            } else if (std::strcmp(event.reason.data(), "pre-overlay-temporal-gate-invalid") == 0) {
                ++m_summary.temporal_gate_skips;
                if (event.capture_phase == CapturePhase::ActiveXeSS) {
                    ++m_summary.active_temporal_gate_skips;
                }
            }
        }
        return event.sequence;
    }

    bool active_capture_started() const noexcept {
        return m_active_capture_started_snapshot.load(std::memory_order_acquire);
    }

    bool claim_first_output_install_window() noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started || m_first_output_install_window_claimed) {
            return false;
        }
        m_first_output_install_window_claimed = true;
        return true;
    }

    bool claim_first_execute_success_window() noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started || m_first_execute_success_window_claimed ||
            !m_first_execute_success_seen) {
            return false;
        }
        m_first_execute_success_window_claimed = true;
        return true;
    }

    bool claim_first_marker_queued_window() noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started || m_first_marker_queued_window_claimed ||
            m_summary.active_marker_queued == 0) {
            return false;
        }
        m_first_marker_queued_window_claimed = true;
        return true;
    }

    bool claim_first_mapping_window() noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started || m_first_mapping_window_claimed ||
            (m_summary.active_mapping_unknown == 0 && m_summary.active_mapping_inferred_candidates == 0)) {
            return false;
        }
        m_first_mapping_window_claimed = true;
        return true;
    }

    bool claim_anomaly_window(AnomalyWindow anomaly) noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started) {
            return false;
        }
        const auto bit = uint32_t{ 1 } << static_cast<uint8_t>(anomaly);
        if ((m_claimed_anomaly_windows & bit) != 0) {
            return false;
        }
        m_claimed_anomaly_windows |= bit;
        return true;
    }

    bool note_pending_present(uint64_t install_id, uint64_t present_ordinal) noexcept {
        std::lock_guard lock{ m_mutex };
        if (!m_active_capture_started || install_id == 0 || present_ordinal == 0) {
            return false;
        }
        if (install_id != m_pending_present_install_id) {
            m_pending_present_install_id = install_id;
            m_pending_present_last_ordinal = 0;
            m_pending_present_count = 0;
            m_pending_present_window_claimed = false;
        }
        if (present_ordinal == m_pending_present_last_ordinal) {
            return false;
        }
        m_pending_present_last_ordinal = present_ordinal;
        ++m_pending_present_count;
        if (m_pending_present_count < 2 || m_pending_present_window_claimed) {
            return false;
        }
        m_pending_present_window_claimed = true;
        return true;
    }

    bool reserve_dump(DumpWindow window, size_t requested_events, DumpReservation& reservation) noexcept {
        constexpr std::array<uint32_t, static_cast<size_t>(DumpWindow::Count)> WINDOW_LIMITS{
            2, 5, 6, 2, 1,
        };
        constexpr uint32_t MAX_DUMP_WINDOWS = 16;
        constexpr size_t MAX_EVENT_LINE_BYTES = 2048;
        constexpr size_t MAX_WINDOW_HEADER_BYTES = 1024;
        std::lock_guard lock{ m_mutex };
        ++m_summary.dump_requests;
        const auto index = static_cast<size_t>(window);
        const bool pre_active_window = window == DumpWindow::PreActive;
        if (index >= WINDOW_LIMITS.size() ||
            (pre_active_window && m_active_capture_started) ||
            (!pre_active_window && !m_active_capture_started) ||
            m_dump_window_counts[index] >= WINDOW_LIMITS[index] ||
            m_dump_windows_reserved >= MAX_DUMP_WINDOWS) {
            ++m_summary.dump_windows_suppressed;
            return false;
        }

        auto event_count = (std::min)(requested_events, MAX_EVENTS_PER_DUMP);
        event_count = (std::min)(event_count, MAX_DUMP_EVENT_LINES - m_dump_event_lines_reserved);
        const auto bytes_available = MAX_DUMP_BYTES - m_dump_bytes_reserved;
        if (bytes_available <= MAX_WINDOW_HEADER_BYTES) {
            ++m_summary.dump_windows_suppressed;
            return false;
        }
        event_count = (std::min)(event_count,
            (bytes_available - MAX_WINDOW_HEADER_BYTES) / MAX_EVENT_LINE_BYTES);
        if (event_count == 0) {
            ++m_summary.dump_windows_suppressed;
            return false;
        }

        ++m_dump_window_counts[index];
        ++m_dump_windows_reserved;
        const auto estimated_bytes = MAX_WINDOW_HEADER_BYTES + event_count * MAX_EVENT_LINE_BYTES;
        m_dump_event_lines_reserved += event_count;
        m_dump_bytes_reserved += estimated_bytes;
        m_summary.dump_event_lines_reserved = m_dump_event_lines_reserved;
        m_summary.dump_bytes_reserved = m_dump_bytes_reserved;
        reservation = DumpReservation{
            window,
            m_dump_windows_reserved,
            event_count,
            estimated_bytes,
            m_active_capture_started,
        };
        return true;
    }

    void note_dump_emitted(const DumpReservation& reservation, size_t event_lines) noexcept {
        std::lock_guard lock{ m_mutex };
        ++m_summary.dump_windows_emitted;
        m_summary.dump_event_lines_emitted += (std::min)(event_lines, reservation.event_count);
        if (reservation.active_phase) {
            ++m_summary.active_gameplay_dump_emitted;
        }
    }

    bool claim_final_summary() noexcept {
        std::lock_guard lock{ m_mutex };
        if (m_final_summary_emitted || (!m_active_capture_started && m_summary.total_events == 0)) {
            return false;
        }
        m_final_summary_emitted = true;
        return true;
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

    struct SceneViewObservation {
        uintptr_t scene_view{};
        float native_width{};
        float native_height{};
        float effective_width{};
        float effective_height{};
        uint32_t input_width{};
        uint32_t input_height{};
        uint32_t display_width{};
        uint32_t display_height{};
        bool input_resolution_valid{};
        bool temporal_active{};
        bool override_applied{};
    };

    struct SceneViewFrameObservations {
        std::array<SceneViewObservation, MAX_SCENE_VIEW_OBSERVATIONS_PER_FRAME> observations{};
        uint64_t frame_id{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        size_t observation_count{};
        bool valid{};
    };

    static SceneViewObservation scene_view_observation(const Event& event) noexcept {
        return SceneViewObservation{
            event.scene_view,
            event.scene_view_native_width,
            event.scene_view_native_height,
            event.scene_view_effective_width,
            event.scene_view_effective_height,
            event.input_width,
            event.input_height,
            event.display_width,
            event.display_height,
            event.input_resolution_valid,
            event.temporal_active,
            event.scene_view_override_applied,
        };
    }

    static bool same_scene_view_observation(
        const SceneViewObservation& left,
        const SceneViewObservation& right) noexcept {
        return left.scene_view == right.scene_view &&
            left.native_width == right.native_width &&
            left.native_height == right.native_height &&
            left.effective_width == right.effective_width &&
            left.effective_height == right.effective_height &&
            left.input_width == right.input_width &&
            left.input_height == right.input_height &&
            left.display_width == right.display_width &&
            left.display_height == right.display_height &&
            left.input_resolution_valid == right.input_resolution_valid &&
            left.temporal_active == right.temporal_active &&
            left.override_applied == right.override_applied;
    }

    static bool scene_view_observation_conflicts(
        const SceneViewObservation& left,
        const SceneViewObservation& right) noexcept {
        return left.native_width != right.native_width ||
            left.native_height != right.native_height ||
            left.effective_width != right.effective_width ||
            left.effective_height != right.effective_height ||
            left.input_width != right.input_width ||
            left.input_height != right.input_height ||
            left.display_width != right.display_width ||
            left.display_height != right.display_height ||
            left.input_resolution_valid != right.input_resolution_valid ||
            left.temporal_active != right.temporal_active ||
            left.override_applied != right.override_applied;
    }

    bool admit_scene_view_observation_locked(Event& event) noexcept {
        SceneViewFrameObservations* frame_state{};
        for (auto& candidate : m_scene_view_frame_observations) {
            if (candidate.valid && candidate.frame_id == event.frame_id &&
                candidate.control_generation == event.control_generation &&
                candidate.device_reset_generation == event.device_reset_generation) {
                frame_state = &candidate;
                break;
            }
        }

        if (frame_state == nullptr) {
            frame_state = &m_scene_view_frame_observations[m_next_scene_view_frame_cache];
            if (frame_state->valid) {
                ++m_summary.scene_view_frame_cache_evictions;
                if (m_active_capture_started) {
                    ++m_summary.active_scene_view_frame_cache_evictions;
                }
            }
            *frame_state = {};
            frame_state->frame_id = event.frame_id;
            frame_state->control_generation = event.control_generation;
            frame_state->device_reset_generation = event.device_reset_generation;
            frame_state->valid = true;
            m_next_scene_view_frame_cache =
                (m_next_scene_view_frame_cache + 1) % SCENE_VIEW_FRAME_CACHE_CAPACITY;
        }

        const auto observation = scene_view_observation(event);
        bool conflict{};
        for (size_t index = 0; index < frame_state->observation_count; ++index) {
            const auto& prior = frame_state->observations[index];
            if (same_scene_view_observation(prior, observation)) {
                return false;
            }
            conflict = conflict || scene_view_observation_conflicts(prior, observation);
        }

        if (frame_state->observation_count >= MAX_SCENE_VIEW_OBSERVATIONS_PER_FRAME) {
            ++m_summary.scene_view_size_observation_drops;
            if (m_active_capture_started) {
                ++m_summary.active_scene_view_size_observation_drops;
            }
            return false;
        }

        frame_state->observations[frame_state->observation_count++] = observation;
        event.scene_view_conflict = conflict;
        if (conflict) {
            ++m_summary.scene_view_size_conflicts;
            if (m_active_capture_started) {
                ++m_summary.active_scene_view_size_conflicts;
            }
        }
        return true;
    }

    RE4XeSSLifetimeTrace() = default;

    mutable std::mutex m_mutex{};
    std::array<Event, CAPACITY> m_events{};
    std::array<InstallState, CAPACITY> m_installs{};
    std::array<uint64_t, static_cast<size_t>(Kind::Count)> m_counts{};
    Summary m_summary{};
    std::atomic<bool> m_enabled{};
    std::atomic<bool> m_active_capture_started_snapshot{};
    std::atomic<uint64_t> m_next_trace_id{};
    uint64_t m_sequence{};
    size_t m_next{};
    size_t m_size{};
    bool m_active_capture_started{};
    bool m_first_execute_success_seen{};
    bool m_first_output_install_window_claimed{};
    bool m_first_execute_success_window_claimed{};
    bool m_first_marker_queued_window_claimed{};
    bool m_first_mapping_window_claimed{};
    bool m_final_summary_emitted{};
    bool m_pending_present_window_claimed{};
    std::array<SceneViewFrameObservations, SCENE_VIEW_FRAME_CACHE_CAPACITY> m_scene_view_frame_observations{};
    size_t m_next_scene_view_frame_cache{};
    uint32_t m_claimed_anomaly_windows{};
    uint64_t m_pending_present_install_id{};
    uint64_t m_pending_present_last_ordinal{};
    uint32_t m_pending_present_count{};
    std::array<uint32_t, static_cast<size_t>(DumpWindow::Count)> m_dump_window_counts{};
    uint32_t m_dump_windows_reserved{};
    size_t m_dump_event_lines_reserved{};
    size_t m_dump_bytes_reserved{};
};
