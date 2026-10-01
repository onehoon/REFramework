// cl /nologo /std:c++latest /EHsc /W4 /I src tests\re4_xess_lifetime_trace_tests.cpp /Fe:re4_xess_lifetime_trace_tests.exe

#include "mods/re4_xess/RE4XeSSLifetimeTrace.hpp"
#include "mods/re4_xess/RE4XeSSSceneViewDecision.hpp"

#include <cassert>
#include <iostream>

using Trace = RE4XeSSLifetimeTrace;

namespace {

uint64_t test_output_use_token_issuance_lifecycle() {
    Trace::OutputUseTokenIssuer issuer;
    assert(issuer.active_token() == 0);

    // A rejected install must neither issue nor expose a token.
    assert(issuer.on_install_result(false, true) == 0);
    assert(issuer.active_token() == 0);

    const auto first_token = issuer.on_install_result(true, true);
    assert(first_token != 0);
    assert(issuer.active_token() == first_token);

    // A rejected attempt cannot replace the token belonging to an outstanding restored handoff.
    assert(issuer.on_install_result(false, true) == 0);
    assert(issuer.active_token() == first_token);

    // Safe generation retirement clears only the active association, not the issuer sequence.
    issuer.retire_generation();
    assert(issuer.active_token() == 0);
    const auto second_token = issuer.on_install_result(true, true);
    assert(second_token != 0);
    assert(second_token == first_token + 1);

    // Successful untraced installs expose zero; re-enabling tracing cannot resurrect a stale token.
    issuer.retire_generation();
    assert(issuer.on_install_result(true, false) == 0);
    assert(issuer.active_token() == 0);
    issuer.retire_generation();
    const auto third_token = issuer.on_install_result(true, true);
    assert(third_token != 0);
    assert(third_token == second_token + 1);
    assert(third_token != first_token);
    return first_token;
}

void assert_latest_event_has_no_reader_token(
    const Trace& trace,
    Trace::Kind expected_kind) {
    const auto recent = trace.recent(1);
    assert(recent.size() == 1);
    assert(recent.front().kind == expected_kind);
    assert(recent.front().output_use_token == 0);
    assert(recent.front().consumer_evidence == Trace::ConsumerEvidence::Unknown);
}

void record_simple(Trace& trace, Trace::Kind kind, uint64_t trace_id = 0) {
    Trace::Event event{};
    event.kind = kind;
    event.trace_id = trace_id;
    event.frame_valid = kind == Trace::Kind::Skip;
    Trace::set_reason(event, "test");
    trace.record(event);
}

void complete_reservation(Trace& trace, Trace::DumpWindow window) {
    Trace::DumpReservation reservation{};
    assert(trace.reserve_dump(window, 100, reservation));
    assert(reservation.event_count == Trace::MAX_EVENTS_PER_DUMP);
    assert(reservation.event_count <= 32);
    trace.note_dump_emitted(reservation, 1);
}

void test_phase_transition_and_correlated_first_submit(Trace& trace, uint64_t output_use_token) {
    record_simple(trace, Trace::Kind::PreOverlay);
    record_simple(trace, Trace::Kind::Skip);
    complete_reservation(trace, Trace::DumpWindow::PreActive);
    complete_reservation(trace, Trace::DumpWindow::PreActive);
    Trace::DumpReservation denied{};
    assert(!trace.reserve_dump(Trace::DumpWindow::PreActive, 32, denied));

    Trace::Event submit{};
    submit.kind = Trace::Kind::Submit;
    submit.trace_id = 42;
    submit.successful_submission = true;
    submit.reset_history = true;
    Trace::set_reason(submit, "submitted");
    trace.record(submit);

    Trace::Event install{};
    install.kind = Trace::Kind::OutputInstall;
    install.trace_id = 42;
    install.install_id = 700;
    install.output_use_token = output_use_token;
    install.consumer_evidence = Trace::ConsumerEvidence::ReaderNotObserved;
    install.mapping_observed = true;
    install.mapping_state = Trace::MappingState::InferredCandidate;
    Trace::set_reason(install, "install");
    trace.record(install);

    Trace::Event context_sample{};
    context_sample.kind = Trace::Kind::OverlayRenderContext;
    context_sample.trace_id = 42;
    context_sample.install_id = 1;
    context_sample.output_resource = 0x2000;
    context_sample.target_state = 0x1000;
    context_sample.render_context = 0x3000;
    context_sample.render_context_target_state = 0x1000;
    context_sample.render_context_target_resource = 0x2000;
    context_sample.overlay_main_target_state = 0x1000;
    context_sample.overlay_main_target_resource = 0x2000;
    context_sample.render_context_sample_valid = true;
    context_sample.render_context_target_state_matches_output = true;
    context_sample.render_context_target_resource_matches_output = true;
    context_sample.render_context_target_state_matches_overlay_main = true;
    context_sample.render_context_target_resource_matches_overlay_main = true;
    context_sample.render_context_stage = Trace::RenderContextStage::BeforeOriginalOverlayDraw;
    Trace::set_reason(context_sample, "test-context-sample-not-submit-proof");
    trace.record(context_sample);

    const auto summary = trace.summary();
    assert(summary.active_capture_started);
    assert(summary.active_output_installs == 1);
    assert(summary.installed_unmarked == 1);
    assert(summary.installed_unmarked_high_water == 1);
    assert(summary.active_successful_submissions == 1);
    assert(summary.active_reset_history_submits == 1);
    assert(summary.active_render_context_samples == 1);
    assert(summary.active_render_context_state_matches == 1);
    assert(summary.active_render_context_resource_matches == 1);
    assert(summary.active_render_context_overlay_main_state_matches == 1);
    assert(summary.active_render_context_overlay_main_resource_matches == 1);
    assert(summary.mapping_proven == 0);
    assert(summary.active_event_counts[static_cast<size_t>(Trace::Kind::Submit)] == 1);
    assert(summary.pre_active_event_counts[static_cast<size_t>(Trace::Kind::Submit)] == 0);
    const auto recent = trace.recent(8);
    assert(recent.size() == 5);
    assert(recent[2].kind == Trace::Kind::Submit);
    assert(recent[2].capture_phase == Trace::CapturePhase::ActiveXeSS);
    assert(recent[3].kind == Trace::Kind::OutputInstall);
    assert(recent[3].capture_phase == Trace::CapturePhase::ActiveXeSS);
    assert(recent[3].install_id == 700);
    assert(recent[3].output_use_token == output_use_token);
    assert(recent[3].consumer_evidence == Trace::ConsumerEvidence::ReaderNotObserved);
    assert(recent[4].kind == Trace::Kind::OverlayRenderContext);
    assert(recent[4].mapping_state == Trace::MappingState::Unknown);
    assert(recent[4].render_context_stage == Trace::RenderContextStage::BeforeOriginalOverlayDraw);
    assert(recent[4].output_use_token == 0);
    assert(recent[4].consumer_evidence == Trace::ConsumerEvidence::Unknown);
    assert(trace.claim_first_output_install_window());
    assert(!trace.claim_first_output_install_window());
    assert(trace.claim_first_execute_success_window());
    assert(!trace.claim_first_execute_success_window());
}

void test_mapping_pending_interval_and_active_budgets(Trace& trace) {
    Trace::Event pending_skip{};
    pending_skip.kind = Trace::Kind::Skip;
    pending_skip.frame_valid = false;
    Trace::set_reason(pending_skip, "output-handoff-marker-pending");
    trace.record(pending_skip);

    Trace::Event post_present{};
    post_present.kind = Trace::Kind::PostPresentCallback;
    post_present.install_id = 700;
    post_present.mapping_observed = true;
    post_present.mapping_state = Trace::MappingState::InferredCandidate;
    assert(post_present.output_use_token == 0);
    assert(post_present.consumer_evidence == Trace::ConsumerEvidence::Unknown);
    assert(post_present.mapping_state == Trace::MappingState::InferredCandidate);
    Trace::set_reason(post_present, "candidate-only");
    trace.record(post_present);
    assert_latest_event_has_no_reader_token(trace, Trace::Kind::PostPresentCallback);
    assert(trace.claim_first_mapping_window());
    assert(!trace.claim_first_mapping_window());

    assert(!trace.note_pending_present(700, 100));
    assert(!trace.note_pending_present(700, 100));
    assert(trace.note_pending_present(700, 101));
    assert(!trace.note_pending_present(700, 102));
    assert(!trace.note_pending_present(2, 1));
    assert(trace.note_pending_present(2, 2));

    Trace::Event marker{};
    marker.kind = Trace::Kind::Marker;
    marker.install_id = 700;
    marker.marker_queued = true;
    marker.mapping_observed = true;
    marker.mapping_state = Trace::MappingState::InferredCandidate;
    assert(marker.output_use_token == 0);
    assert(marker.consumer_evidence == Trace::ConsumerEvidence::Unknown);
    assert(marker.mapping_state == Trace::MappingState::InferredCandidate);
    Trace::set_reason(marker, "marker-queued");
    trace.record(marker);
    assert_latest_event_has_no_reader_token(trace, Trace::Kind::Marker);
    auto summary = trace.summary();
    assert(summary.installed_unmarked == 0);
    assert(summary.installed_unmarked_high_water == 1);
    assert(summary.marked_gpu_incomplete == 1);
    assert(trace.claim_first_marker_queued_window());
    assert(!trace.claim_first_marker_queued_window());

    Trace::Event completion{};
    completion.kind = Trace::Kind::DownstreamFenceComplete;
    completion.install_id = 700;
    completion.marker_queued = true;
    completion.actual_completed_value = 1;
    completion.actual_completed_value_valid = true;
    assert(completion.output_use_token == 0);
    assert(completion.consumer_evidence == Trace::ConsumerEvidence::Unknown);
    Trace::set_reason(completion, "downstream-complete");
    trace.record(completion);
    assert_latest_event_has_no_reader_token(trace, Trace::Kind::DownstreamFenceComplete);
    summary = trace.summary();
    assert(summary.installed_unmarked == 0);
    assert(summary.installed_unmarked_high_water == 1);
    assert(summary.marked_gpu_incomplete == 0);

    for (int i = 0; i < 5; ++i) complete_reservation(trace, Trace::DumpWindow::ActiveMilestone);
    Trace::DumpReservation denied{};
    assert(!trace.reserve_dump(Trace::DumpWindow::ActiveMilestone, 32, denied));
    for (int i = 0; i < 6; ++i) complete_reservation(trace, Trace::DumpWindow::Anomaly);
    assert(!trace.reserve_dump(Trace::DumpWindow::Anomaly, 32, denied));
    for (int i = 0; i < 2; ++i) complete_reservation(trace, Trace::DumpWindow::ModeTransition);
    assert(!trace.reserve_dump(Trace::DumpWindow::ModeTransition, 32, denied));
    complete_reservation(trace, Trace::DumpWindow::Periodic);
    assert(!trace.reserve_dump(Trace::DumpWindow::Periodic, 32, denied));
    assert(trace.claim_anomaly_window(Trace::AnomalyWindow::FirstMarkerPending));
    assert(!trace.claim_anomaly_window(Trace::AnomalyWindow::FirstMarkerPending));
    assert(trace.claim_anomaly_window(Trace::AnomalyWindow::MarkerPendingAcrossPresent));
    assert(!trace.claim_anomaly_window(Trace::AnomalyWindow::MarkerPendingAcrossPresent));
    assert(trace.claim_anomaly_window(Trace::AnomalyWindow::WriterOrFenceFailure));
    assert(!trace.claim_anomaly_window(Trace::AnomalyWindow::WriterOrFenceFailure));

    summary = trace.summary();
    assert(summary.dump_windows_emitted == 16);
    assert(summary.dump_event_lines_reserved == 512);
    assert(summary.dump_bytes_reserved <= Trace::MAX_DUMP_BYTES);
    assert(summary.active_gameplay_dump_emitted == 14);
    assert(summary.mapping_inferred_candidates == 3);
    assert(summary.active_mapping_inferred_candidates == 3);
    assert(summary.mapping_proven == 0);
    assert(summary.active_marker_pending_skips == 1);
    assert(summary.dump_windows_suppressed >= 5);
}

void test_scene_view_override_decision() {
    const auto active = RE4XeSSSceneView::decide_size(true, 2560.0f, 1440.0f, true, true, 1706, 960);
    assert(active.override_applied);
    assert(active.effective_width == 1706.0f && active.effective_height == 960.0f);

    const auto temporal_off = RE4XeSSSceneView::decide_size(true, 2560.0f, 1440.0f, false, true, 1706, 960);
    assert(!temporal_off.override_applied);
    assert(temporal_off.effective_width == 2560.0f && temporal_off.effective_height == 1440.0f);

    const auto load_unavailable = RE4XeSSSceneView::decide_size(true, 2560.0f, 1440.0f, true, false, 1706, 960);
    assert(!load_unavailable.override_applied);
    assert(load_unavailable.effective_width == 2560.0f && load_unavailable.effective_height == 1440.0f);

    const auto invalid_input_extent = RE4XeSSSceneView::decide_size(true, 2560.0f, 1440.0f, true, true, 0, 960);
    assert(!invalid_input_extent.override_applied);
    assert(invalid_input_extent.effective_width == 2560.0f && invalid_input_extent.effective_height == 1440.0f);

    const auto missing_result = RE4XeSSSceneView::decide_size(false, 0.0f, 0.0f, true, true, 1706, 960);
    assert(!missing_result.override_applied);
    assert(missing_result.effective_width == 0.0f && missing_result.effective_height == 0.0f);
}

void test_scene_extent_and_jitter_trace(Trace& trace) {
    const auto before = trace.summary();
    const auto scene_view_count_before =
        before.active_event_counts[static_cast<size_t>(Trace::Kind::SceneViewSize)];

    const auto make_view_size = [](uint64_t frame_id, uintptr_t scene_view) {
        Trace::Event event{};
        event.kind = Trace::Kind::SceneViewSize;
        event.frame_id = frame_id;
        event.frame_valid = true;
        event.scene_view = scene_view;
        event.scene_view_native_width = 2560.0f;
        event.scene_view_native_height = 1440.0f;
        event.scene_view_effective_width = 1706.0f;
        event.scene_view_effective_height = 960.0f;
        event.input_width = 1706;
        event.input_height = 960;
        event.display_width = 2560;
        event.display_height = 1440;
        event.input_resolution_valid = true;
        event.temporal_active = true;
        event.scene_view_override_applied = true;
        event.control_generation = 8;
        event.device_reset_generation = 3;
        Trace::set_reason(event, "reduced-scene-view-applied");
        return event;
    };

    auto view_size = make_view_size(99001, 0x1234);
    assert(trace.record(view_size) != 0);
    assert(trace.record(view_size) == 0); // Exact same-view/value duplicate is suppressed.

    // A distinct SceneView identity in the same frame is retained even when the sizes match.
    auto second_view = make_view_size(99001, 0x5678);
    assert(trace.record(second_view) != 0);

    // A changed extent for the same view is retained and explicitly marked as a conflict.
    auto changed_extent = view_size;
    changed_extent.scene_view_native_width = 1920.0f;
    changed_extent.scene_view_native_height = 1080.0f;
    changed_extent.scene_view_effective_width = 1280.0f;
    changed_extent.scene_view_effective_height = 720.0f;
    changed_extent.input_width = 1280;
    changed_extent.input_height = 720;
    assert(trace.record(changed_extent) != 0);

    // A changed override flag with unchanged dimensions is separately visible and conflict-marked.
    auto changed_override = changed_extent;
    changed_override.scene_view_override_applied = false;
    Trace::set_reason(changed_override, "temporal-view-gate-closed");
    assert(trace.record(changed_override) != 0);
    assert(trace.record(changed_override) == 0);

    // Preserve a temporal-state discrepancy even when dimensions/override fields otherwise match.
    auto changed_temporal_state = view_size;
    changed_temporal_state.temporal_active = false;
    assert(trace.record(changed_temporal_state) != 0);

    // The same frame/view under a new control or device generation is not deduplicated.
    auto new_control_generation = view_size;
    new_control_generation.control_generation = 9;
    assert(trace.record(new_control_generation) != 0);
    auto new_device_generation = view_size;
    new_device_generation.device_reset_generation = 4;
    assert(trace.record(new_device_generation) != 0);

    // Correlate a reduced SceneView/jitter sample with a subsequent marker-pending skip.
    constexpr uint64_t skip_frame_id = 99002;
    auto reduced_view = make_view_size(skip_frame_id, 0x9abc);
    reduced_view.control_generation = 10;
    reduced_view.device_reset_generation = 6;
    assert(trace.record(reduced_view) != 0);

    Trace::Event scene_frame{};
    scene_frame.kind = Trace::Kind::SceneFrame;
    scene_frame.frame_id = skip_frame_id;
    scene_frame.frame_valid = true;
    scene_frame.control_generation = reduced_view.control_generation;
    scene_frame.device_reset_generation = reduced_view.device_reset_generation;
    scene_frame.input_width = 1706;
    scene_frame.input_height = 960;
    scene_frame.display_width = 2560;
    scene_frame.display_height = 1440;
    scene_frame.input_resolution_valid = true;
    scene_frame.jitter_applied = true;
    scene_frame.jitter_x_pixels = 0.25f;
    scene_frame.jitter_y_pixels = -0.166667f;
    scene_frame.jitter_sample_index = 4;
    scene_frame.jitter_phase_count = 16;
    Trace::set_reason(scene_frame, "primary-scene-jitter-applied");
    assert(trace.record(scene_frame) != 0);

    Trace::Event pending_skip{};
    pending_skip.kind = Trace::Kind::Skip;
    pending_skip.frame_id = skip_frame_id;
    pending_skip.frame_valid = true;
    pending_skip.control_generation = reduced_view.control_generation;
    pending_skip.device_reset_generation = reduced_view.device_reset_generation;
    Trace::set_reason(pending_skip, "output-handoff-marker-pending");
    assert(trace.record(pending_skip) != 0);

    // The per-frame sample set is fixed; overflow is explicit in the summary.
    constexpr uint64_t capped_frame_id = 99003;
    for (size_t index = 0; index < Trace::MAX_SCENE_VIEW_OBSERVATIONS_PER_FRAME; ++index) {
        auto sample = make_view_size(capped_frame_id, static_cast<uintptr_t>(0x10000 + index));
        sample.control_generation = 10;
        sample.device_reset_generation = 6;
        assert(trace.record(sample) != 0);
    }
    auto dropped_sample = make_view_size(capped_frame_id, 0x20000);
    dropped_sample.control_generation = 10;
    dropped_sample.device_reset_generation = 6;
    assert(trace.record(dropped_sample) == 0);

    // Evicting an old frame-key window is also visible instead of silently claiming full coverage.
    for (uint64_t frame_id = 99100; frame_id < 99110; ++frame_id) {
        auto sample = make_view_size(frame_id, 0x30000);
        sample.control_generation = 10;
        sample.device_reset_generation = 6;
        assert(trace.record(sample) != 0);
    }

    const auto recent = trace.recent(Trace::CAPACITY);
    const auto find_event = [&](Trace::Kind kind, uint64_t frame_id, uint64_t control, uint64_t device) {
        return std::find_if(recent.begin(), recent.end(), [&](const auto& event) {
            return event.kind == kind && event.frame_id == frame_id &&
                event.control_generation == control && event.device_reset_generation == device;
        });
    };
    const auto retained_second_view = std::find_if(recent.begin(), recent.end(), [](const auto& event) {
        return event.kind == Trace::Kind::SceneViewSize && event.frame_id == 99001 &&
            event.control_generation == 8 && event.device_reset_generation == 3 &&
            event.scene_view == 0x5678;
    });
    assert(retained_second_view != recent.end());
    const auto retained_changed_extent = std::find_if(recent.begin(), recent.end(), [](const auto& event) {
        return event.kind == Trace::Kind::SceneViewSize && event.frame_id == 99001 &&
            event.scene_view == 0x1234 && event.scene_view_native_width == 1920.0f;
    });
    assert(retained_changed_extent != recent.end() && retained_changed_extent->scene_view_conflict);
    const auto retained_changed_override = std::find_if(recent.begin(), recent.end(), [](const auto& event) {
        return event.kind == Trace::Kind::SceneViewSize && event.frame_id == 99001 &&
            event.scene_view == 0x1234 && !event.scene_view_override_applied;
    });
    assert(retained_changed_override != recent.end() && retained_changed_override->scene_view_conflict);
    const auto retained_changed_temporal_state = std::find_if(recent.begin(), recent.end(), [](const auto& event) {
        return event.kind == Trace::Kind::SceneViewSize && event.frame_id == 99001 &&
            event.scene_view == 0x1234 && !event.temporal_active;
    });
    assert(retained_changed_temporal_state != recent.end() && retained_changed_temporal_state->scene_view_conflict);

    const auto retained_scene_frame = find_event(
        Trace::Kind::SceneFrame, skip_frame_id, reduced_view.control_generation, reduced_view.device_reset_generation);
    const auto retained_skip = find_event(
        Trace::Kind::Skip, skip_frame_id, reduced_view.control_generation, reduced_view.device_reset_generation);
    assert(retained_scene_frame != recent.end() && retained_scene_frame->jitter_applied);
    assert(retained_scene_frame->input_width == 1706 && retained_scene_frame->jitter_sample_index == 4);
    assert(retained_scene_frame->output_use_token == 0);
    assert(retained_scene_frame->consumer_evidence == Trace::ConsumerEvidence::Unknown);
    assert(retained_skip != recent.end());
    assert(std::strcmp(retained_skip->reason.data(), "output-handoff-marker-pending") == 0);
    const auto retained_reduced_view = find_event(
        Trace::Kind::SceneViewSize, skip_frame_id, reduced_view.control_generation, reduced_view.device_reset_generation);
    assert(retained_reduced_view != recent.end() && retained_reduced_view->scene_view_override_applied);

    const auto summary = trace.summary();
    assert(summary.active_event_counts[static_cast<size_t>(Trace::Kind::SceneViewSize)] - scene_view_count_before == 26);
    assert(summary.active_scene_view_size_conflicts == before.active_scene_view_size_conflicts + 3);
    assert(summary.active_scene_view_size_observation_drops == before.active_scene_view_size_observation_drops + 1);
    assert(summary.active_scene_view_frame_cache_evictions > before.active_scene_view_frame_cache_evictions);
    assert(summary.active_event_counts[static_cast<size_t>(Trace::Kind::SceneFrame)] == 1);
}

} // namespace

int main() {
    auto& trace = Trace::instance();
    trace.configure(true);
    const auto output_use_token = test_output_use_token_issuance_lifecycle();
    test_phase_transition_and_correlated_first_submit(trace, output_use_token);
    test_mapping_pending_interval_and_active_budgets(trace);
    test_scene_view_override_decision();
    test_scene_extent_and_jitter_trace(trace);
    assert(trace.claim_final_summary());
    assert(!trace.claim_final_summary());
    std::cout << "RE4 XeSS lifetime-trace tests passed\n";
}
