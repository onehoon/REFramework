// cl /nologo /std:c++latest /EHsc /W4 /I src tests\re4_xess_lifetime_trace_tests.cpp /Fe:re4_xess_lifetime_trace_tests.exe

#include "mods/re4_xess/RE4XeSSLifetimeTrace.hpp"

#include <cassert>
#include <iostream>

using Trace = RE4XeSSLifetimeTrace;

namespace {

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

void test_phase_transition_and_correlated_first_submit(Trace& trace) {
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
    install.install_id = 1;
    install.mapping_observed = true;
    install.mapping_state = Trace::MappingState::InferredCandidate;
    Trace::set_reason(install, "install");
    trace.record(install);

    const auto summary = trace.summary();
    assert(summary.active_capture_started);
    assert(summary.active_output_installs == 1);
    assert(summary.installed_unmarked == 1);
    assert(summary.installed_unmarked_high_water == 1);
    assert(summary.active_successful_submissions == 1);
    assert(summary.active_reset_history_submits == 1);
    assert(summary.active_event_counts[static_cast<size_t>(Trace::Kind::Submit)] == 1);
    assert(summary.pre_active_event_counts[static_cast<size_t>(Trace::Kind::Submit)] == 0);
    const auto recent = trace.recent(8);
    assert(recent.size() == 4);
    assert(recent[2].kind == Trace::Kind::Submit);
    assert(recent[2].capture_phase == Trace::CapturePhase::ActiveXeSS);
    assert(recent[3].kind == Trace::Kind::OutputInstall);
    assert(recent[3].capture_phase == Trace::CapturePhase::ActiveXeSS);
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
    post_present.install_id = 1;
    post_present.mapping_observed = true;
    post_present.mapping_state = Trace::MappingState::InferredCandidate;
    Trace::set_reason(post_present, "candidate-only");
    trace.record(post_present);
    assert(trace.claim_first_mapping_window());
    assert(!trace.claim_first_mapping_window());

    assert(!trace.note_pending_present(1, 100));
    assert(!trace.note_pending_present(1, 100));
    assert(trace.note_pending_present(1, 101));
    assert(!trace.note_pending_present(1, 102));
    assert(!trace.note_pending_present(2, 1));
    assert(trace.note_pending_present(2, 2));

    Trace::Event marker{};
    marker.kind = Trace::Kind::Marker;
    marker.install_id = 1;
    marker.marker_queued = true;
    marker.mapping_observed = true;
    marker.mapping_state = Trace::MappingState::InferredCandidate;
    Trace::set_reason(marker, "marker-queued");
    trace.record(marker);
    auto summary = trace.summary();
    assert(summary.installed_unmarked == 0);
    assert(summary.installed_unmarked_high_water == 1);
    assert(summary.marked_gpu_incomplete == 1);
    assert(trace.claim_first_marker_queued_window());
    assert(!trace.claim_first_marker_queued_window());

    Trace::Event completion{};
    completion.kind = Trace::Kind::DownstreamFenceComplete;
    completion.install_id = 1;
    completion.marker_queued = true;
    completion.actual_completed_value = 1;
    completion.actual_completed_value_valid = true;
    Trace::set_reason(completion, "downstream-complete");
    trace.record(completion);
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

} // namespace

int main() {
    auto& trace = Trace::instance();
    trace.configure(true);
    test_phase_transition_and_correlated_first_submit(trace);
    test_mapping_pending_interval_and_active_budgets(trace);
    assert(trace.claim_final_summary());
    assert(!trace.claim_final_summary());
    std::cout << "RE4 XeSS lifetime-trace tests passed\n";
}
