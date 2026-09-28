#include "mods/re4_xess/RE4XeSSOutputHandoff.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "mods/REFrameworkConfig.hpp"

namespace {

constexpr auto OUTPUT_READ_STATE = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
constexpr auto REQUIRED_OUTPUT_FLAGS = static_cast<D3D12_RESOURCE_FLAGS>(
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
constexpr uint32_t MODE_TRANSITION_OBSERVATION_BUDGET = 4;
constexpr uint32_t TRANSITION_PROVENANCE_EVENT_BUDGET = 24;
constexpr uintptr_t RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET = 0x90;
std::atomic<uint64_t> transition_provenance_sequence{};

struct RetainedOutputGeneration {
    sdk::intrusive_ptr<sdk::renderer::TargetState> handoff_state{};
    sdk::intrusive_ptr<sdk::renderer::TargetState> saved_original_state{};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource{};
    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence{};
};

std::string retirement_status_message(RE4XeSSOutputHandoff::RetirementStatus status) {
    switch (status) {
    case RE4XeSSOutputHandoff::RetirementStatus::NoGeneration: return "no previous output generation";
    case RE4XeSSOutputHandoff::RetirementStatus::Active: return "output generation is active";
    case RE4XeSSOutputHandoff::RetirementStatus::WriterPending: return "bridge writer fence is still draining";
    case RE4XeSSOutputHandoff::RetirementStatus::MissingMarker: return "waiting for a same-generation downstream retirement marker";
    case RE4XeSSOutputHandoff::RetirementStatus::Draining: return "downstream retirement marker is queued and draining";
    case RE4XeSSOutputHandoff::RetirementStatus::Ready: return "previous output generation retired";
    case RE4XeSSOutputHandoff::RetirementStatus::Quarantined: return "output generation is quarantined";
    case RE4XeSSOutputHandoff::RetirementStatus::DeviceRemoved: return "old D3D12 device generation was removed";
    }
    return "unknown output retirement state";
}

bool valid_texture_extent(
    const D3D12_RESOURCE_DESC& description,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format) {
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height &&
        description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
        description.Format == format;
}

}

RE4XeSSOutputHandoff::~RE4XeSSOutputHandoff() {
    if (m_provenance_enabled.load(std::memory_order_acquire)) {
        std::lock_guard lock{ m_provenance_mutex };
        spdlog::info("[RE4XeSS][HandoffProvenance] summary events={} divergences={} invalidSlotReads={} writerWritesCaptured=0 writerInstrumentation=not-armed reason=no-validated-writer-site",
            static_cast<unsigned long long>(m_provenance_event_count),
            static_cast<unsigned long long>(m_provenance_divergence_count),
            static_cast<unsigned long long>(m_provenance_invalid_slot_count));
    }
    if (m_handoff_state != nullptr || m_device != nullptr || m_queue != nullptr || m_retirement_fence != nullptr) {
        preserve_quarantined_generation();
    }
}

bool RE4XeSSOutputHandoff::restore(
    sdk::renderer::layer::Overlay* layer,
    const ObservationContext& observation,
    std::string& error) {
    error.clear();
    if (m_identity_mismatch_latched.load(std::memory_order_acquire)) {
        error = "Overlay main TargetState identity mismatch is terminally quarantined";
        return false;
    }
    if (!m_installed) {
        return true;
    }

    if (layer == nullptr) {
        error = "Overlay layer is unavailable; the installed handoff remains retained for a later pre-Overlay restore";
        return false;
    }
    if (layer != m_installed_overlay) {
        {
            std::lock_guard lock{ m_retirement_mutex };
            m_installed = false;
            m_installed_snapshot.store(false, std::memory_order_release);
            m_installed_overlay = nullptr;
            m_retirement_requested = true;
            m_missing_marker = m_marker_pending;
            m_retirement_status = m_marker_pending
                ? RetirementStatus::MissingMarker
                : RetirementStatus::Draining;
            m_failure_reason = "Overlay generation changed while a handoff was installed; old Overlay was not dereferenced";
        }
        spdlog::warn("[RE4XeSS][Output] Overlay generation changed; old target state remains retained for retirement");
        return true;
    }

    auto& main_state = layer->get_main_target_state();
    auto* const observed_target_state = main_state.get();
    log_transition_provenance(
        "pre-overlay-entry", observation,
        reinterpret_cast<uintptr_t>(layer), reinterpret_cast<uintptr_t>(observed_target_state), true, 1u << 1);
    const bool transition_observation = consume_mode_transition_observation(observation);
    if (observed_target_state == m_handoff_state.get()) {
        main_state = m_saved_original_state;
    } else if (observed_target_state != m_saved_original_state.get()) {
        error = "Overlay main TargetState changed to an unexpected object while the XeSS handoff was installed";
        bool first_mismatch{};
        uintptr_t installed_layer_identity{};
        uintptr_t installed_state_identity{};
        uintptr_t saved_state_identity{};
        uintptr_t template_state_identity{};
        uintptr_t last_confirmed_overlay_layer{};
        uintptr_t last_confirmed_overlay_state{};
        uint64_t installed_frame{};
        uint64_t last_confirmed_overlay_frame{};
        uint64_t installed_control_generation{};
        uint64_t installed_device_reset_generation{};
        uint64_t signaled_fence_value{};
        uint64_t completed_fence_value{};
        int32_t installed_mode_token{};
        bool installed{};
        bool marker_pending{};
        bool retirement_requested{};
        bool hard_quarantined{};
        bool bridge_writer_uncertain{};
        bool last_confirmed_overlay_valid{};
        bool same_layer_as_last_confirmed{};
        uint64_t actual_fence_value{};
        bool actual_fence_read{};
        {
            std::lock_guard lock{ m_retirement_mutex };
            if (!m_identity_mismatch_latched.exchange(true, std::memory_order_acq_rel)) {
                first_mismatch = true;
                m_hard_quarantined = true;
                m_retirement_requested = true;
                m_retirement_status = RetirementStatus::Quarantined;
                m_failure_reason = error;
                m_quarantine_logged = true;
            }
            installed_layer_identity = reinterpret_cast<uintptr_t>(m_installed_overlay);
            installed_state_identity = reinterpret_cast<uintptr_t>(m_handoff_state.get());
            saved_state_identity = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
            template_state_identity = m_signature.template_state;
            installed_frame = m_installed_frame;
            last_confirmed_overlay_layer = m_last_confirmed_post_overlay_layer;
            last_confirmed_overlay_state = m_last_confirmed_post_overlay_state;
            last_confirmed_overlay_frame = m_last_confirmed_post_overlay_frame;
            last_confirmed_overlay_valid = m_last_confirmed_post_overlay_valid;
            same_layer_as_last_confirmed = last_confirmed_overlay_valid &&
                last_confirmed_overlay_layer == reinterpret_cast<uintptr_t>(layer);
            installed_control_generation = m_signature.control_generation;
            installed_device_reset_generation = m_installed_device_reset_generation;
            installed_mode_token = m_installed_mode_token;
            signaled_fence_value = m_last_signaled_retirement_value;
            completed_fence_value = m_last_completed_retirement_value;
            installed = m_installed;
            marker_pending = m_marker_pending;
            retirement_requested = m_retirement_requested;
            hard_quarantined = m_hard_quarantined;
            bridge_writer_uncertain = m_bridge_writer_uncertain;
            if (first_mismatch && m_retirement_fence != nullptr) {
                actual_fence_value = m_retirement_fence->GetCompletedValue();
                actual_fence_read = true;
            }
        }
        if (first_mismatch) {
            const auto target_state_slot = reinterpret_cast<uintptr_t>(&main_state);
            const auto target_state_slot_offset = target_state_slot - reinterpret_cast<uintptr_t>(layer);
            spdlog::error(
                "[RE4XeSS][OutputHandoffMismatch] phase=pre-overlay-restore tid={} frameKnown={} frame={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} installedModeToken={} installedControlGeneration={} installedDeviceResetGeneration={} installedFrame={} currentLayer=0x{:x} installedLayer=0x{:x} targetStateSlot=0x{:x} targetStateSlotOffset=0x{:x} sameLayerAsLastConfirmedPostOverlay={} observedMainTargetState=0x{:x} observedTargetRelation=third-object expectedHandoffState=0x{:x} savedOriginalState=0x{:x} lastTemplateTargetState=0x{:x} lastConfirmedPostOverlayValid={} lastConfirmedPostOverlayFrame={} lastConfirmedPostOverlayLayer=0x{:x} lastConfirmedPostOverlayTargetState=0x{:x} installed={} markerPending={} retirementRequested={} hardQuarantined={} bridgeWriterUncertain={} downstreamFenceLastSignaled={} cachedFenceCompleted={} actualFenceRead={} actualFenceCompleted={} actualFenceResult={}",
                observation.callback_thread_id,
                observation.frame_id_valid,
                static_cast<unsigned long long>(observation.frame_id),
                observation.requested_mode_token,
                static_cast<unsigned long long>(observation.control_generation),
                static_cast<unsigned long long>(observation.device_reset_generation),
                installed_mode_token,
                static_cast<unsigned long long>(installed_control_generation),
                static_cast<unsigned long long>(installed_device_reset_generation),
                static_cast<unsigned long long>(installed_frame),
                reinterpret_cast<uintptr_t>(layer),
                installed_layer_identity,
                target_state_slot,
                target_state_slot_offset,
                same_layer_as_last_confirmed,
                reinterpret_cast<uintptr_t>(observed_target_state),
                installed_state_identity,
                saved_state_identity,
                template_state_identity,
                last_confirmed_overlay_valid,
                static_cast<unsigned long long>(last_confirmed_overlay_frame),
                last_confirmed_overlay_layer,
                last_confirmed_overlay_state,
                installed,
                marker_pending,
                retirement_requested,
                hard_quarantined,
                bridge_writer_uncertain,
                static_cast<unsigned long long>(signaled_fence_value),
                static_cast<unsigned long long>(completed_fence_value),
                actual_fence_read,
                static_cast<unsigned long long>(actual_fence_value),
                !actual_fence_read ? "unavailable" :
                    actual_fence_value == std::numeric_limits<uint64_t>::max() ? "device-removed-or-invalid" :
                    actual_fence_value >= signaled_fence_value ? "completed-through-last-signal" : "not-yet-complete");
            log_transition_provenance(
                "pre-overlay-restore-rejected", observation,
                reinterpret_cast<uintptr_t>(layer), reinterpret_cast<uintptr_t>(observed_target_state), true, 1u << 3);
        }
        return false;
    }

    if (transition_observation) {
        uintptr_t expected_state_identity{};
        uintptr_t saved_state_identity{};
        uint64_t installed_frame{};
        uint64_t signaled_fence_value{};
        uint64_t completed_fence_value{};
        bool marker_pending{};
        {
            std::lock_guard lock{ m_retirement_mutex };
            expected_state_identity = reinterpret_cast<uintptr_t>(m_handoff_state.get());
            saved_state_identity = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
            installed_frame = m_installed_frame;
            marker_pending = m_marker_pending;
            signaled_fence_value = m_last_signaled_retirement_value;
            completed_fence_value = m_last_completed_retirement_value;
        }
        spdlog::info(
            "[RE4XeSS][ModeTransition] phase=pre-overlay-restore tid={} frameKnown={} frame={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} layer=0x{:x} observedMainTargetState=0x{:x} expectedHandoffState=0x{:x} savedOriginalState=0x{:x} installedFrame={} markerPending={} lastSignaled={} lastCompleted={}",
            observation.callback_thread_id,
            observation.frame_id_valid,
            static_cast<unsigned long long>(observation.frame_id),
            observation.requested_mode_token,
            static_cast<unsigned long long>(observation.control_generation),
            static_cast<unsigned long long>(observation.device_reset_generation),
            reinterpret_cast<uintptr_t>(layer),
            reinterpret_cast<uintptr_t>(observed_target_state),
            expected_state_identity,
            saved_state_identity,
            static_cast<unsigned long long>(installed_frame),
            marker_pending,
            static_cast<unsigned long long>(signaled_fence_value),
            static_cast<unsigned long long>(completed_fence_value));
    }

    bool log_missing_marker{};
    uint64_t missing_marker_frame{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_installed = false;
        m_installed_snapshot.store(false, std::memory_order_release);
        m_installed_overlay = nullptr;
        if (m_marker_pending) {
            m_missing_marker = true;
            m_retirement_status = RetirementStatus::MissingMarker;
            if (m_failure_reason.empty()) {
                m_failure_reason = "The previous installed handoff frame has no post-Present retirement marker yet";
            }
            m_marker_wait_logged = true;
            if (m_delayed_marker_log_count < 8) {
                ++m_delayed_marker_log_count;
                log_missing_marker = true;
                missing_marker_frame = m_installed_frame;
            }
        } else if (m_retirement_requested) {
            m_retirement_status = RetirementStatus::Draining;
        }
    }
    if (log_missing_marker) {
        spdlog::warn("[RE4XeSS][Output] restored handoff frame={} without its post-Present marker; next submission/install is blocked until same-generation settlement",
            static_cast<unsigned long long>(missing_marker_frame));
    }
    bool log_restore{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        log_restore = REFrameworkConfig::get()->is_debug_log_enabled() && m_restore_log_count < 32;
        if (log_restore) {
            ++m_restore_log_count;
        }
    }
    if (log_restore) {
        spdlog::info("[RE4XeSS][Output] restored original Overlay main state before frame resource collection; frame={}",
            static_cast<unsigned long long>(m_installed_frame));
    }
    log_transition_provenance(
        "after-restore", observation,
        reinterpret_cast<uintptr_t>(layer),
        reinterpret_cast<uintptr_t>(layer->get_main_target_state().get()), true, 1u << 2);
    return true;
}

void RE4XeSSOutputHandoff::request_retirement(std::string_view reason) {
    bool changed{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (m_handoff_state == nullptr) {
            return;
        }
        changed = !m_retirement_requested;
        m_retirement_requested = true;
        if (m_retirement_status == RetirementStatus::Active) {
            m_retirement_status = m_marker_pending
                ? RetirementStatus::MissingMarker
                : RetirementStatus::Draining;
        }
        if (changed && !reason.empty()) {
            m_failure_reason = std::string{ reason };
        }
    }
    if (changed) {
        spdlog::info("[RE4XeSS][Output] retirement requested: {}", reason.empty() ? "generation transition" : reason);
    }
}

void RE4XeSSOutputHandoff::note_mode_transition(
    uint64_t control_generation,
    int32_t requested_mode_token,
    uint64_t device_reset_generation) noexcept {
    m_mode_transition_generation.store(control_generation, std::memory_order_release);
    m_mode_transition_observation_budget.store(MODE_TRANSITION_OBSERVATION_BUDGET, std::memory_order_release);
    m_provenance_phase_mask.store(0, std::memory_order_release);
    m_provenance_event_budget.store(TRANSITION_PROVENANCE_EVENT_BUDGET, std::memory_order_release);
    m_provenance_generation.store(control_generation, std::memory_order_release);
    {
        std::lock_guard lock{ m_provenance_mutex };
        m_provenance_divergence_count = 0;
    }
    const ObservationContext observation{
        requested_mode_token,
        control_generation,
        device_reset_generation,
        0,
        GetCurrentThreadId(),
        false,
    };
    log_transition_provenance("mode-request", observation, 0, 0, false, 1u << 0);
}

void RE4XeSSOutputHandoff::configure_transition_provenance(
    bool enabled,
    std::string_view reason,
    uintptr_t image_base,
    uint32_t image_size,
    uint32_t image_checksum) noexcept {
    m_provenance_enabled.store(enabled, std::memory_order_release);
    spdlog::info(
        "[RE4XeSS][HandoffProvenance] armed={} reason={} imageBase=0x{:x} imageSize=0x{:x} imageChecksum=0x{:x} activation=DebugLog+RE4XeSS_HandoffProvenance writerInstrumentation=not-armed writerReason=no-validated-writer-site",
        enabled,
        reason,
        image_base,
        image_size,
        image_checksum);
}

void RE4XeSSOutputHandoff::log_transition_provenance(
    std::string_view phase,
    const ObservationContext& observation,
    uintptr_t layer_identity,
    uintptr_t current_state_identity,
    bool current_state_known,
    uint32_t phase_bit) noexcept {
    if (!m_provenance_enabled.load(std::memory_order_acquire) || observation.control_generation == 0 ||
        observation.control_generation != m_provenance_generation.load(std::memory_order_acquire) ||
        REFrameworkConfig::get() == nullptr || !REFrameworkConfig::get()->is_debug_log_enabled()) {
        return;
    }
    const auto previous_mask = m_provenance_phase_mask.fetch_or(phase_bit, std::memory_order_acq_rel);
    const bool first_phase = (previous_mask & phase_bit) == 0;
    if (!first_phase) {
        return;
    }

    uintptr_t installed_layer{};
    uintptr_t handoff_state{};
    uintptr_t saved_state{};
    uintptr_t last_confirmed_layer{};
    uintptr_t last_confirmed_state{};
    uint64_t installed_frame{};
    uint64_t signaled{};
    uint64_t cached_completed{};
    uint64_t installed_control_generation{};
    uint64_t installed_device_generation{};
    int32_t installed_mode_token{};
    bool installed{};
    bool marker_pending{};
    bool retirement_requested{};
    bool hard_quarantined{};
    bool bridge_writer_uncertain{};
    bool last_confirmed_valid{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        installed_layer = reinterpret_cast<uintptr_t>(m_installed_overlay);
        handoff_state = reinterpret_cast<uintptr_t>(m_handoff_state.get());
        saved_state = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
        last_confirmed_layer = m_last_confirmed_post_overlay_layer;
        last_confirmed_state = m_last_confirmed_post_overlay_state;
        installed_frame = m_installed_frame;
        signaled = m_last_signaled_retirement_value;
        cached_completed = m_last_completed_retirement_value;
        installed_control_generation = m_signature.control_generation;
        installed_device_generation = m_installed_device_reset_generation;
        installed_mode_token = m_installed_mode_token;
        installed = m_installed;
        marker_pending = m_marker_pending;
        retirement_requested = m_retirement_requested;
        hard_quarantined = m_hard_quarantined;
        bridge_writer_uncertain = m_bridge_writer_uncertain;
        last_confirmed_valid = m_last_confirmed_post_overlay_valid;
    }

    uintptr_t target_state_slot{};
    uintptr_t observed_current_state{};
    bool slot_read_valid{};
    if (current_state_known && layer_identity != 0 &&
        layer_identity <= UINTPTR_MAX - RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET) {
        auto* const layer = reinterpret_cast<sdk::renderer::layer::Overlay*>(layer_identity);
        const auto accessor_slot = reinterpret_cast<uintptr_t>(&layer->get_main_target_state());
        target_state_slot = layer_identity + RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET;
        if (accessor_slot == target_state_slot) {
            SIZE_T bytes_read{};
            slot_read_valid = ReadProcessMemory(
                GetCurrentProcess(), reinterpret_cast<const void*>(target_state_slot),
                &observed_current_state, sizeof(observed_current_state), &bytes_read) != FALSE &&
                bytes_read == sizeof(observed_current_state);
        }
    }
    if (current_state_known && layer_identity != 0 && !slot_read_valid) {
        std::lock_guard lock{ m_provenance_mutex };
        ++m_provenance_invalid_slot_count;
    }
    auto previous_current_state = slot_read_valid
        ? m_provenance_last_current_state.exchange(observed_current_state, std::memory_order_acq_rel)
        : 0;
    const bool identity_changed = slot_read_valid && previous_current_state != 0 &&
        previous_current_state != observed_current_state;
    bool first_divergence = slot_read_valid && installed && observed_current_state != handoff_state &&
        observed_current_state != saved_state;
    if (!slot_read_valid) {
        previous_current_state = m_provenance_last_current_state.load(std::memory_order_acquire);
    }

    if (!identity_changed && !first_divergence && !first_phase) {
        return;
    }
    auto remaining = m_provenance_event_budget.load(std::memory_order_acquire);
    while (remaining != 0 && !m_provenance_event_budget.compare_exchange_weak(
        remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if (remaining == 0) {
        return;
    }

    const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ProvenanceSample sample{
        transition_provenance_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        timestamp_us,
        observation.control_generation,
        observation.device_reset_generation,
        observation.frame_id,
        observation.callback_thread_id,
        observation.requested_mode_token,
        installed_mode_token,
        layer_identity,
        target_state_slot,
        slot_read_valid ? observed_current_state : current_state_identity,
        handoff_state,
        saved_state,
        observation.frame_id_valid,
        slot_read_valid,
        identity_changed,
        marker_pending,
        installed,
        retirement_requested,
        hard_quarantined,
    };
    std::array<ProvenanceSample, 8> preceding{};
    size_t preceding_count{};
    {
        std::lock_guard lock{ m_provenance_mutex };
        ++m_provenance_event_count;
        if (slot_read_valid) {
            m_provenance_ring[m_provenance_ring_next] = sample;
            m_provenance_ring_next = (m_provenance_ring_next + 1) % m_provenance_ring.size();
            m_provenance_ring_size = std::min(m_provenance_ring_size + 1, m_provenance_ring.size());
        }
        if (first_divergence) {
            if (m_provenance_divergence_count == 0) {
                ++m_provenance_divergence_count;
            } else {
                first_divergence = false;
            }
        }
        if (first_divergence) {
            preceding_count = m_provenance_ring_size;
            const auto first = (m_provenance_ring_next + m_provenance_ring.size() - preceding_count) % m_provenance_ring.size();
            for (size_t i = 0; i < preceding_count; ++i) {
                preceding[i] = m_provenance_ring[(first + i) % m_provenance_ring.size()];
            }
        }
    }
    spdlog::info(
        "[RE4XeSS][OutputTransition] seq={} timestampUs={} phase={} tid={} frameKnown={} frame={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} installedModeToken={} installedControlGeneration={} installedDeviceResetGeneration={} currentLayer=0x{:x} installedLayer=0x{:x} targetStateSlot=0x{:x} targetStateSlotOffset=0x{:x} currentKnown={} slotReadValid={} currentTargetState=0x{:x} expectedHandoffState=0x{:x} savedOriginalState=0x{:x} lastConfirmedValid={} lastConfirmedLayer=0x{:x} lastConfirmedTargetState=0x{:x} installedFrame={} installed={} markerPending={} retirementRequested={} hardQuarantined={} bridgeWriterUncertain={} downstreamFenceLastSignaled={} cachedFenceCompleted={} identityChanged={} firstDivergence={} previousCurrentTargetState=0x{:x}",
        static_cast<unsigned long long>(sample.sequence),
        static_cast<long long>(timestamp_us),
        phase,
        observation.callback_thread_id,
        observation.frame_id_valid,
        static_cast<unsigned long long>(observation.frame_id),
        observation.requested_mode_token,
        static_cast<unsigned long long>(observation.control_generation),
        static_cast<unsigned long long>(observation.device_reset_generation),
        installed_mode_token,
        static_cast<unsigned long long>(installed_control_generation),
        static_cast<unsigned long long>(installed_device_generation),
        layer_identity,
        installed_layer,
        target_state_slot,
        target_state_slot != 0 ? target_state_slot - layer_identity : 0,
        current_state_known,
        slot_read_valid,
        slot_read_valid ? observed_current_state : current_state_identity,
        handoff_state,
        saved_state,
        last_confirmed_valid,
        last_confirmed_layer,
        last_confirmed_state,
        static_cast<unsigned long long>(installed_frame),
        installed,
        marker_pending,
        retirement_requested,
        hard_quarantined,
        bridge_writer_uncertain,
        static_cast<unsigned long long>(signaled),
        static_cast<unsigned long long>(cached_completed),
        identity_changed,
        first_divergence,
        previous_current_state);

    if (first_divergence) {
        spdlog::error("[RE4XeSS][HandoffProvenance] first-divergence seq={} current=0x{:x} expected=0x{:x} saved=0x{:x} precedingSamples={}; writer=unknown",
            static_cast<unsigned long long>(sample.sequence), observed_current_state, handoff_state, saved_state, preceding_count);
        for (size_t i = 0; i < preceding_count; ++i) {
            const auto& prior = preceding[i];
            spdlog::info("[RE4XeSS][HandoffProvenanceSample] seq={} timestampUs={} tid={} frameKnown={} frame={} controlGeneration={} deviceResetGeneration={} mode={} overlay=0x{:x} slot=0x{:x} slotReadValid={} current=0x{:x} expected=0x{:x} saved=0x{:x} markerPending={} installed={} retirementRequested={} hardQuarantined={} identityChanged={}",
                static_cast<unsigned long long>(prior.sequence), prior.timestamp_us, prior.thread_id,
                prior.frame_id_valid, static_cast<unsigned long long>(prior.frame_id),
                static_cast<unsigned long long>(prior.control_generation),
                static_cast<unsigned long long>(prior.device_reset_generation), prior.requested_mode_token,
                prior.overlay, prior.slot, prior.slot_read_valid, prior.current_state,
                prior.expected_state, prior.saved_state, prior.marker_pending, prior.installed,
                prior.retirement_requested, prior.hard_quarantined, prior.identity_changed);
        }
    }
}

bool RE4XeSSOutputHandoff::consume_mode_transition_observation(
    const ObservationContext& observation) noexcept {
    if (observation.control_generation == 0 ||
        observation.control_generation != m_mode_transition_generation.load(std::memory_order_acquire)) {
        return false;
    }

    auto remaining = m_mode_transition_observation_budget.load(std::memory_order_acquire);
    while (remaining != 0) {
        if (m_mode_transition_observation_budget.compare_exchange_weak(
                remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool RE4XeSSOutputHandoff::identity_mismatch_latched() const noexcept {
    return m_identity_mismatch_latched.load(std::memory_order_acquire);
}

RE4XeSSOutputHandoff::RetirementStatus RE4XeSSOutputHandoff::poll_retirement(
    bool bridge_writer_idle,
    bool bridge_device_removed) {
    if (m_handoff_state == nullptr) {
        return RetirementStatus::NoGeneration;
    }

    RetirementStatus status{};
    bool release{};
    bool log_status{};
    std::string status_reason;
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (!m_retirement_requested) {
            return RetirementStatus::Active;
        }
        if (m_installed) {
            m_hard_quarantined = true;
            if (!m_identity_mismatch_latched.load(std::memory_order_acquire)) {
                m_failure_reason = "Refusing to retire an output TargetState while it is still installed in Overlay";
            }
        }

        if (confirmed_device_removal(bridge_device_removed)) {
            m_device_removed = true;
            m_hard_quarantined = false;
            m_bridge_writer_uncertain = false;
            m_marker_pending = false;
            m_installed = false;
            m_installed_snapshot.store(false, std::memory_order_release);
            m_installed_overlay = nullptr;
            status = RetirementStatus::DeviceRemoved;
            release = true;
        } else {
            bool writer_quarantine_pending{};
            if (m_bridge_writer_uncertain) {
                if (!bridge_writer_idle) {
                    writer_quarantine_pending = true;
                    status = RetirementStatus::Quarantined;
                    m_failure_reason = "XeSS writer completion remains quarantined by the bridge generation";
                } else {
                    m_bridge_writer_uncertain = false;
                    m_failure_reason.clear();
                }
            }

            if (!writer_quarantine_pending) {
                if (m_hard_quarantined) {
                    status = RetirementStatus::Quarantined;
                } else if (!bridge_writer_idle) {
                    status = RetirementStatus::WriterPending;
                } else if (m_installed) {
                    m_hard_quarantined = true;
                    if (!m_identity_mismatch_latched.load(std::memory_order_acquire)) {
                        m_failure_reason = "Refusing to retire an output TargetState while it is still installed in Overlay";
                    }
                    status = RetirementStatus::Quarantined;
                } else if (!m_downstream_use_seen) {
                    status = RetirementStatus::Ready;
                    release = true;
                } else if (m_marker_pending || m_last_signaled_retirement_value == 0) {
                    m_missing_marker = true;
                    status = RetirementStatus::MissingMarker;
                } else if (m_retirement_fence == nullptr) {
                    m_hard_quarantined = true;
                    m_failure_reason = "Downstream retirement fence disappeared before lifetime proof";
                    status = RetirementStatus::Quarantined;
                } else {
                    const auto completed = m_retirement_fence->GetCompletedValue();
                    if (completed == std::numeric_limits<uint64_t>::max()) {
                        if (confirmed_device_removal(false)) {
                            m_device_removed = true;
                            status = RetirementStatus::DeviceRemoved;
                            release = true;
                        } else {
                            m_hard_quarantined = true;
                            m_failure_reason = "Retirement fence returned UINT64_MAX without confirmed device removal";
                            status = RetirementStatus::Quarantined;
                        }
                    } else {
                        m_last_completed_retirement_value = completed;
                        if (completed >= m_last_signaled_retirement_value) {
                            status = RetirementStatus::Ready;
                            release = true;
                        } else {
                            status = RetirementStatus::Draining;
                        }
                    }
                }
            }
        }
        m_retirement_status = status;
        status_reason = m_failure_reason;
        if (status == RetirementStatus::Quarantined && !m_quarantine_logged) {
            m_quarantine_logged = true;
            log_status = true;
        } else if (status == RetirementStatus::MissingMarker && !m_missing_marker_logged) {
            m_missing_marker_logged = true;
            log_status = true;
        }
        if (log_status) {
            ++m_retirement_log_count;
        }
    }

    if (release) {
        release_generation();
        if (status == RetirementStatus::DeviceRemoved) {
            spdlog::warn("[RE4XeSS][Output] old output generation released after confirmed device removal");
        } else {
            spdlog::info("[RE4XeSS][Output] old handoff generation retired; bridge writer and downstream consumer proofs are complete");
        }
    } else if (status == RetirementStatus::MissingMarker || status == RetirementStatus::Quarantined) {
        if (log_status) {
            spdlog::warn("[RE4XeSS][Output] {}{}",
                retirement_status_message(status),
                status_reason.empty() ? std::string{} : ": " + status_reason);
        }
    }
    return status;
}

RE4XeSSOutputHandoff::PrepareResult RE4XeSSOutputHandoff::prepare(
    sdk::renderer::layer::Overlay* layer,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Resource* semantic_color,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t display_width,
    uint32_t display_height,
    uint64_t control_generation,
    bool bridge_writer_idle,
    bool bridge_device_removed,
    RE4XeSSD3D12::OutputBinding& output,
    std::string& error) {
    output = {};
    error.clear();
    if (layer == nullptr || device == nullptr || queue == nullptr || semantic_color == nullptr) {
        error = "The RE4 XeSS handoff is missing Overlay, device, DIRECT queue, or semantic Color";
        return PrepareResult::Failed;
    }

    auto* current_template = layer->get_main_target_state().get();
    if (current_template == nullptr) {
        error = "Overlay main TargetState is unavailable for XeSS output handoff";
        return PrepareResult::Failed;
    }
    const auto color_description = semantic_color->GetDesc();
    Signature requested_signature{
        reinterpret_cast<uintptr_t>(current_template),
        reinterpret_cast<uintptr_t>(semantic_color),
        reinterpret_cast<uintptr_t>(device),
        reinterpret_cast<uintptr_t>(queue),
        control_generation,
        display_width,
        display_height,
        color_description.Format,
    };

    bool existing_generation_usable{};
    bool waiting_for_marker{};
    bool log_marker_block{};
    uint64_t waiting_marker_frame{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        waiting_for_marker = m_marker_pending || m_missing_marker;
        existing_generation_usable = !m_retirement_requested && !m_hard_quarantined &&
            !m_bridge_writer_uncertain && !m_device_removed && !waiting_for_marker;
        if (waiting_for_marker) {
            m_missing_marker = true;
            m_retirement_status = RetirementStatus::MissingMarker;
            if (!m_marker_wait_logged && m_delayed_marker_log_count < 8) {
                m_marker_wait_logged = true;
                ++m_delayed_marker_log_count;
                log_marker_block = true;
                waiting_marker_frame = m_installed_frame;
            }
        }
    }
    if (m_handoff_state != nullptr && m_signature == requested_signature && waiting_for_marker) {
        error = "Previous RE4 XeSS handoff frame is waiting for its downstream post-Present retirement marker";
        if (log_marker_block) {
            spdlog::info("[RE4XeSS][Output] transiently skipping producer frame while same-generation frame={} awaits post-Present settlement; no new XeSS output submit/install",
                static_cast<unsigned long long>(waiting_marker_frame));
        }
        return PrepareResult::WaitingForPostPresentMarker;
    }
    if (m_handoff_state != nullptr && m_signature == requested_signature && existing_generation_usable) {
        output.resource = m_resource_pin.Get();
        output.before_state = m_expected_state;
        output.after_state = OUTPUT_READ_STATE;
        if (output.resource == nullptr) {
            error = "The active RE4 XeSS output handoff lost its native resource pin";
            return PrepareResult::Failed;
        }
        return PrepareResult::Ready;
    }

    if (m_handoff_state != nullptr) {
        request_retirement("handoff template/device/queue/mode/display generation changed");
        const auto status = poll_retirement(bridge_writer_idle, bridge_device_removed);
        if (status != RetirementStatus::Ready && status != RetirementStatus::DeviceRemoved &&
            status != RetirementStatus::NoGeneration) {
            error = "Previous RE4 XeSS output generation is not reusable: " + retirement_status_message(status);
            const auto state = snapshot();
            if (!state.failure_reason.empty()) {
                error += "; " + state.failure_reason;
            }
            return PrepareResult::Failed;
        }
    }

    if (!validate_and_clone(
            layer,
            device,
            queue,
            semantic_color,
            render_width,
            render_height,
            display_width,
            display_height,
            error)) {
        return PrepareResult::Failed;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    const auto fence_result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(fence_result)) {
        error = "CreateFence(downstream retirement) failed with HRESULT " +
            std::to_string(static_cast<uint32_t>(fence_result));
        m_handoff_state.reset();
        m_resource_pin.Reset();
        return PrepareResult::Failed;
    }
    m_resource_pin->SetName(L"RE4XeSS Handoff Output");

    m_signature = requested_signature;
    m_expected_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_retirement_value = 1;
    m_last_signaled_retirement_value = 0;
    m_last_completed_retirement_value = 0;
    m_installed_frame = 0;
    m_installed_device_reset_generation = 0;
    m_installed_mode_token = 0;
    m_last_confirmed_post_overlay_layer = 0;
    m_last_confirmed_post_overlay_state = 0;
    m_last_confirmed_post_overlay_frame = 0;
    m_last_confirmed_post_overlay_valid = false;
    m_retirement_log_count = 0;
    m_restore_log_count = 0;
    m_delayed_marker_log_count = 0;
    m_installed = false;
    m_installed_overlay = nullptr;
    m_saved_original_state.reset();
    m_retirement_requested = false;
    m_downstream_use_seen = false;
    m_marker_pending = false;
    m_missing_marker = false;
    m_marker_wait_logged = false;
    m_marker_recovery_pending = false;
    m_hard_quarantined = false;
    m_bridge_writer_uncertain = false;
    m_missing_marker_logged = false;
    m_quarantine_logged = false;
    m_post_overlay_mismatch_logged = false;
    m_device_removed = false;
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_device = device;
        m_queue = queue;
        m_retirement_fence = std::move(fence);
        m_failure_reason.clear();
        m_retirement_status = RetirementStatus::Active;
        m_has_generation_snapshot.store(true, std::memory_order_release);
        m_installed_snapshot.store(false, std::memory_order_release);
    }
    spdlog::info("[RE4XeSS][Output] cloned display handoff target state=0x{:x} resource=0x{:x} render={}x{} display={}x{} firstState=COMMON",
        reinterpret_cast<uintptr_t>(m_handoff_state.get()),
        reinterpret_cast<uintptr_t>(m_resource_pin.Get()),
        render_width,
        render_height,
        display_width,
        display_height);

    output.resource = m_resource_pin.Get();
    output.before_state = m_expected_state;
    output.after_state = OUTPUT_READ_STATE;
    return PrepareResult::Ready;
}

bool RE4XeSSOutputHandoff::validate_and_clone(
    sdk::renderer::layer::Overlay* layer,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Resource* semantic_color,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t display_width,
    uint32_t display_height,
    std::string& error) {
    if (render_width == 0 || render_height == 0 || display_width == 0 || display_height == 0) {
        error = "The RE4 XeSS handoff extents must be non-zero";
        return false;
    }
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "The RE4 XeSS output handoff requires the active DIRECT queue";
        return false;
    }

    auto& current_state = layer->get_main_target_state();
    if (current_state == nullptr || current_state->get_rtv_count() != 1 ||
        current_state->get_native_resource_d3d12() != semantic_color) {
        error = "Overlay main must be the single-RTV semantic Color TargetState before handoff";
        return false;
    }
    const auto source_description = semantic_color->GetDesc();
    if (!valid_texture_extent(source_description, render_width, render_height, DXGI_FORMAT_R11G11B10_FLOAT) ||
        (source_description.Flags & REQUIRED_OUTPUT_FLAGS) != REQUIRED_OUTPUT_FLAGS) {
        error = "The semantic Color source must be render-resolution R11G11B10_FLOAT with render-target and UAV flags";
        return false;
    }

    std::vector<std::array<uint32_t, 2>> dimensions{
        std::array<uint32_t, 2>{ display_width, display_height }
    };
    auto cloned_state = current_state->clone(dimensions);
    if (cloned_state == nullptr || cloned_state.get() == current_state.get() ||
        cloned_state->get_rtv_count() != 1) {
        error = "TargetState::clone did not produce a distinct single-RTV handoff target";
        return false;
    }
    auto* resource = cloned_state->get_native_resource_d3d12();
    if (resource == nullptr || resource == semantic_color) {
        error = "The cloned handoff TargetState has no distinct native D3D12 resource";
        return false;
    }
    const auto output_description = resource->GetDesc();
    if (!valid_texture_extent(output_description, display_width, display_height, DXGI_FORMAT_R11G11B10_FLOAT) ||
        (output_description.Flags & REQUIRED_OUTPUT_FLAGS) != REQUIRED_OUTPUT_FLAGS) {
        error = "The cloned handoff resource is not a display-resolution R11G11B10_FLOAT render-target/UAV Texture2D";
        return false;
    }
    const auto& rect = cloned_state->get_desc().rect;
    if (rect.left != 0.0f || rect.top != 0.0f ||
        rect.right != static_cast<float>(display_width) || rect.bottom != static_cast<float>(display_height)) {
        error = "The cloned handoff TargetState rect does not match the display extent";
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    const auto support_result = device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    constexpr auto required_support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    if (FAILED(support_result) || (support.Support1 & required_support1) != required_support1 ||
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        error = "The cloned handoff resource format lacks the typed UAV support required by XeSS";
        return false;
    }

    m_handoff_state = std::move(cloned_state);
    m_resource_pin = resource;
    return true;
}

void RE4XeSSOutputHandoff::note_submission_succeeded() {
    m_expected_state = OUTPUT_READ_STATE;
}

bool RE4XeSSOutputHandoff::install(
    sdk::renderer::layer::Overlay* layer,
    const ObservationContext& observation,
    std::string& error) {
    error.clear();
    if (layer == nullptr || m_handoff_state == nullptr || m_resource_pin == nullptr) {
        error = "The RE4 XeSS output generation is unavailable for Overlay installation";
        return false;
    }
    const auto install_entry_state = reinterpret_cast<uintptr_t>(layer->get_main_target_state().get());
    log_transition_provenance("install-entry", observation,
        reinterpret_cast<uintptr_t>(layer), install_entry_state, true, 1u << 4);
    std::unique_lock lock{ m_retirement_mutex };
    if (m_retirement_requested || m_hard_quarantined || m_bridge_writer_uncertain || m_device_removed) {
        error = "The RE4 XeSS output generation is retiring or quarantined";
        return false;
    }
    const auto previous_frame = m_installed_frame;
    const bool log_marker_recovery = m_marker_recovery_pending && m_delayed_marker_log_count < 8;
    if (m_marker_recovery_pending) {
        m_marker_recovery_pending = false;
        if (log_marker_recovery) {
            ++m_delayed_marker_log_count;
        }
    }
    auto& main_state = layer->get_main_target_state();
    if (main_state.get() != reinterpret_cast<sdk::renderer::TargetState*>(m_signature.template_state) ||
        m_expected_state != OUTPUT_READ_STATE) {
        error = "Overlay main or output state changed before the post-submit handoff install";
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = error;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] output handoff quarantined: {}", error);
        return false;
    }

    m_saved_original_state = main_state;
    main_state = m_handoff_state;
    m_installed_overlay = layer;
    m_installed = true;
    m_installed_frame = observation.frame_id;
    m_installed_mode_token = observation.requested_mode_token;
    m_installed_device_reset_generation = observation.device_reset_generation;
    m_downstream_use_seen = true;
    m_marker_pending = true;
    m_missing_marker = false;
    m_installed_snapshot.store(true, std::memory_order_release);
    m_retirement_status = RetirementStatus::Active;
    m_failure_reason.clear();
    const auto installed_state_identity = reinterpret_cast<uintptr_t>(m_handoff_state.get());
    lock.unlock();
    bool log_install{};
    {
        std::lock_guard log_lock{ m_retirement_mutex };
        log_install = REFrameworkConfig::get()->is_debug_log_enabled() && m_retirement_log_count < 32;
        if (log_install) {
            ++m_retirement_log_count;
        }
    }
    if (log_install) {
        spdlog::info("[RE4XeSS][Output] installed handoff frame={} tid={} modeToken={} controlGeneration={} deviceResetGeneration={} overlay=0x{:x} template=0x{:x} savedOriginalState=0x{:x} state=0x{:x} resource=0x{:x} extent={}x{} state=0xC0",
            static_cast<unsigned long long>(observation.frame_id),
            observation.callback_thread_id,
            observation.requested_mode_token,
            static_cast<unsigned long long>(observation.control_generation),
            static_cast<unsigned long long>(observation.device_reset_generation),
            reinterpret_cast<uintptr_t>(layer),
            m_signature.template_state,
            reinterpret_cast<uintptr_t>(m_saved_original_state.get()),
            reinterpret_cast<uintptr_t>(m_handoff_state.get()),
            reinterpret_cast<uintptr_t>(m_resource_pin.Get()),
            m_signature.display_width,
            m_signature.display_height);
    }
    log_transition_provenance("install-complete", observation,
        reinterpret_cast<uintptr_t>(layer), installed_state_identity, true, 1u << 5);
    if (log_marker_recovery) {
        spdlog::info("[RE4XeSS][Output] resumed handoff after frame={} marker settlement; installed frame={}",
            static_cast<unsigned long long>(previous_frame),
            static_cast<unsigned long long>(observation.frame_id));
    }
    return true;
}

void RE4XeSSOutputHandoff::quarantine(std::string_view reason, bool bridge_writer_uncertain) noexcept {
    std::string failure;
    bool log_quarantine{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        const bool same_terminal_quarantine =
            m_retirement_requested &&
            m_retirement_status == RetirementStatus::Quarantined &&
            m_bridge_writer_uncertain == bridge_writer_uncertain &&
            (reason.empty() || m_failure_reason == reason);
        m_bridge_writer_uncertain = bridge_writer_uncertain;
        m_hard_quarantined = !bridge_writer_uncertain;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        if (!reason.empty() && !same_terminal_quarantine) {
            m_failure_reason.assign(reason);
        }
        failure = m_failure_reason;
        log_quarantine = !same_terminal_quarantine;
        m_quarantine_logged = true;
    }
    if (log_quarantine) {
        spdlog::error("[RE4XeSS][Failure] output handoff quarantined: {}", failure);
    }
}

void RE4XeSSOutputHandoff::on_post_present(
    ID3D12Device* active_device,
    ID3D12CommandQueue* active_queue,
    const ObservationContext& observation) noexcept {
    log_transition_provenance("post-present-marker-attempt", observation, 0, 0, false, 1u << 8);
    const auto log_marker_return = [&] {
        log_transition_provenance("post-present-marker-return", observation, 0, 0, false, 1u << 9);
    };
    if (REFrameworkConfig::get()->is_debug_log_enabled() &&
        consume_mode_transition_observation(observation)) {
        uintptr_t installed_layer_identity{};
        uintptr_t installed_state_identity{};
        uintptr_t saved_state_identity{};
        uint64_t installed_frame{};
        uint64_t signaled_fence_value{};
        uint64_t completed_fence_value{};
        bool installed{};
        bool marker_pending{};
        bool retirement_requested{};
        bool hard_quarantined{};
        bool bridge_writer_uncertain{};
        {
            std::lock_guard observation_lock{ m_retirement_mutex };
            installed_layer_identity = reinterpret_cast<uintptr_t>(m_installed_overlay);
            installed_state_identity = reinterpret_cast<uintptr_t>(m_handoff_state.get());
            saved_state_identity = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
            installed_frame = m_installed_frame;
            signaled_fence_value = m_last_signaled_retirement_value;
            completed_fence_value = m_last_completed_retirement_value;
            installed = m_installed;
            marker_pending = m_marker_pending;
            retirement_requested = m_retirement_requested;
            hard_quarantined = m_hard_quarantined;
            bridge_writer_uncertain = m_bridge_writer_uncertain;
        }
        spdlog::info(
            "[RE4XeSS][ModeTransition] phase=post-present-marker-state tid={} frameKnown={} frame={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} installedFrame={} installedLayer=0x{:x} handoffState=0x{:x} savedOriginalState=0x{:x} installed={} markerPending={} retirementRequested={} hardQuarantined={} bridgeWriterUncertain={} lastSignaled={} lastCompleted={}",
            observation.callback_thread_id,
            observation.frame_id_valid,
            static_cast<unsigned long long>(observation.frame_id),
            observation.requested_mode_token,
            static_cast<unsigned long long>(observation.control_generation),
            static_cast<unsigned long long>(observation.device_reset_generation),
            static_cast<unsigned long long>(installed_frame),
            installed_layer_identity,
            installed_state_identity,
            saved_state_identity,
            installed,
            marker_pending,
            retirement_requested,
            hard_quarantined,
            bridge_writer_uncertain,
            static_cast<unsigned long long>(signaled_fence_value),
            static_cast<unsigned long long>(completed_fence_value));
    }

    std::unique_lock lock{ m_retirement_mutex };
    if (!m_has_generation_snapshot.load(std::memory_order_acquire) || !m_marker_pending ||
        m_hard_quarantined || m_retirement_fence == nullptr || m_queue == nullptr || m_device == nullptr) {
        lock.unlock();
        log_marker_return();
        return;
    }
    if (active_device != m_device.Get() || active_queue != m_queue.Get()) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "post-Present active device/queue does not match the handoff generation";
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] downstream retirement marker rejected: {}", failure);
        log_marker_return();
        return;
    }
    if (m_next_retirement_value == 0 || m_next_retirement_value == std::numeric_limits<uint64_t>::max()) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "Downstream retirement fence value space is exhausted";
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] downstream retirement marker rejected: {}", failure);
        log_marker_return();
        return;
    }

    const bool settled_missing_marker = m_missing_marker;
    const auto value = m_next_retirement_value++;
    const auto result = m_queue->Signal(m_retirement_fence.Get(), value);
    if (FAILED(result)) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "post-Present downstream retirement Signal failed with HRESULT " +
            std::to_string(static_cast<uint32_t>(result));
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] {}", failure);
        log_marker_return();
        return;
    }

    m_last_signaled_retirement_value = value;
    m_marker_pending = false;
    m_missing_marker = false;
    m_marker_wait_logged = false;
    if (settled_missing_marker) {
        m_marker_recovery_pending = true;
    }
    m_retirement_status = m_retirement_requested ? RetirementStatus::Draining : RetirementStatus::Active;
    const bool log_marker = m_retirement_log_count < 32;
    if (log_marker) {
        ++m_retirement_log_count;
    }
    const auto frame = m_installed_frame;
    const auto queue_identity = reinterpret_cast<uintptr_t>(m_queue.Get());
    const bool log_marker_settlement = settled_missing_marker && m_delayed_marker_log_count < 8;
    if (log_marker_settlement) {
        ++m_delayed_marker_log_count;
    }
    lock.unlock();
    if (log_marker) {
        spdlog::info("[RE4XeSS][Output] downstream retirement marker queued value={} frame={} queue=0x{:x}",
            static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(frame),
            queue_identity);
    }
    if (log_marker_settlement) {
        spdlog::info("[RE4XeSS][Output] queued delayed-marker settlement for restored handoff frame={} value={}; no additional output reader was installed before this marker",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(value));
    }
    log_transition_provenance("post-present-marker", observation, 0, 0, false, 1u << 7);
    log_marker_return();
}

RE4XeSSOutputHandoff::Snapshot RE4XeSSOutputHandoff::snapshot() const {
    std::lock_guard lock{ m_retirement_mutex };
    return {
        m_has_generation_snapshot.load(std::memory_order_acquire),
        m_installed_snapshot.load(std::memory_order_acquire),
        m_retirement_status,
        m_failure_reason,
    };
}

void RE4XeSSOutputHandoff::observe_overlay(
    sdk::renderer::layer::Overlay* layer,
    const ObservationContext& observation) {
    if (layer == nullptr) {
        return;
    }
    const bool debug_log = REFrameworkConfig::get()->is_debug_log_enabled();
    uintptr_t expected_state{};
    uintptr_t saved_state{};
    uintptr_t resource_identity{};
    uintptr_t installed_layer_identity{};
    uint64_t frame{};
    uint64_t control_generation{};
    uint64_t installed_device_reset_generation{};
    int32_t installed_mode_token{};
    uint32_t width{};
    uint32_t height{};
    bool should_log{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (!m_installed || layer != m_installed_overlay) {
            return;
        }
        should_log = debug_log && m_retirement_log_count < 32;
        if (should_log) {
            ++m_retirement_log_count;
        }
        expected_state = reinterpret_cast<uintptr_t>(m_handoff_state.get());
        saved_state = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
        resource_identity = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
        installed_layer_identity = reinterpret_cast<uintptr_t>(m_installed_overlay);
        frame = m_installed_frame;
        control_generation = m_signature.control_generation;
        installed_device_reset_generation = m_installed_device_reset_generation;
        installed_mode_token = m_installed_mode_token;
        width = m_signature.display_width;
        height = m_signature.display_height;
    }
    const auto& main_state = layer->get_main_target_state();
    const auto observed_state = reinterpret_cast<uintptr_t>(main_state.get());
    log_transition_provenance("post-overlay-observation", observation,
        reinterpret_cast<uintptr_t>(layer), observed_state, true, 1u << 6);
    const bool still_installed = observed_state == expected_state;
    const char* observed_target_relation = still_installed
        ? "handoff"
        : observed_state == saved_state ? "saved-original" : "third-object";
    bool log_mismatch{};
    if (still_installed) {
        std::lock_guard lock{ m_retirement_mutex };
        if (m_installed && layer == m_installed_overlay &&
            expected_state == reinterpret_cast<uintptr_t>(m_handoff_state.get()) &&
            frame == m_installed_frame) {
            m_last_confirmed_post_overlay_layer = reinterpret_cast<uintptr_t>(layer);
            m_last_confirmed_post_overlay_state = observed_state;
            m_last_confirmed_post_overlay_frame = frame;
            m_last_confirmed_post_overlay_valid = true;
        }
    } else if (debug_log) {
        std::lock_guard lock{ m_retirement_mutex };
        if (!m_post_overlay_mismatch_logged) {
            m_post_overlay_mismatch_logged = true;
            log_mismatch = true;
        }
    }
    if (still_installed && should_log) {
        spdlog::info("[RE4XeSS][Output] post-Overlay identity frame={} tid={} modeToken={} controlGeneration={} deviceResetGeneration={} layer=0x{:x} installedLayer=0x{:x} observedMainTargetState=0x{:x} handoffState=0x{:x} savedOriginalState=0x{:x} resource=0x{:x} extent={}x{}",
            static_cast<unsigned long long>(frame),
            observation.callback_thread_id,
            installed_mode_token,
            static_cast<unsigned long long>(control_generation),
            static_cast<unsigned long long>(installed_device_reset_generation),
            reinterpret_cast<uintptr_t>(layer),
            installed_layer_identity,
            observed_state,
            expected_state,
            saved_state,
            resource_identity,
            width,
            height);
    } else if (log_mismatch) {
        uintptr_t last_confirmed_layer{};
        uintptr_t last_confirmed_state{};
        uint64_t last_confirmed_frame{};
        bool last_confirmed_valid{};
        bool same_layer_as_last_confirmed{};
        {
            std::lock_guard lock{ m_retirement_mutex };
            last_confirmed_layer = m_last_confirmed_post_overlay_layer;
            last_confirmed_state = m_last_confirmed_post_overlay_state;
            last_confirmed_frame = m_last_confirmed_post_overlay_frame;
            last_confirmed_valid = m_last_confirmed_post_overlay_valid;
            same_layer_as_last_confirmed = last_confirmed_valid &&
                last_confirmed_layer == reinterpret_cast<uintptr_t>(layer);
        }
        spdlog::error("[RE4XeSS][OutputHandoffMismatch] phase=post-overlay-observation frameKnown={} frame={} tid={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} installedFrame={} currentLayer=0x{:x} installedLayer=0x{:x} sameLayerAsLastConfirmedPostOverlay={} observedMainTargetState=0x{:x} observedTargetRelation={} expectedHandoffState=0x{:x} savedOriginalState=0x{:x} lastConfirmedPostOverlayValid={} lastConfirmedPostOverlayFrame={} lastConfirmedPostOverlayLayer=0x{:x} lastConfirmedPostOverlayTargetState=0x{:x}",
            observation.frame_id_valid,
            static_cast<unsigned long long>(observation.frame_id),
            observation.callback_thread_id,
            observation.requested_mode_token,
            static_cast<unsigned long long>(observation.control_generation),
            static_cast<unsigned long long>(observation.device_reset_generation),
            static_cast<unsigned long long>(frame),
            reinterpret_cast<uintptr_t>(layer),
            installed_layer_identity,
            same_layer_as_last_confirmed,
            observed_state,
            observed_target_relation,
            expected_state,
            saved_state,
            last_confirmed_valid,
            static_cast<unsigned long long>(last_confirmed_frame),
            last_confirmed_layer,
            last_confirmed_state);
    }
}

void RE4XeSSOutputHandoff::release_generation() noexcept {
    sdk::intrusive_ptr<sdk::renderer::TargetState> handoff_state;
    sdk::intrusive_ptr<sdk::renderer::TargetState> saved_original_state;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (m_installed || m_marker_pending || m_bridge_writer_uncertain ||
            (m_hard_quarantined && !m_device_removed)) {
            return;
        }
        m_has_generation_snapshot.store(false, std::memory_order_release);
        m_installed_snapshot.store(false, std::memory_order_release);
        m_retirement_status = RetirementStatus::NoGeneration;
        m_marker_pending = false;
        m_missing_marker = false;
        m_retirement_requested = false;
        m_failure_reason.clear();
        handoff_state = std::move(m_handoff_state);
        saved_original_state = std::move(m_saved_original_state);
        resource = std::move(m_resource_pin);
        device = std::move(m_device);
        queue = std::move(m_queue);
        fence = std::move(m_retirement_fence);
    }
    m_signature = {};
    m_expected_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_retirement_value = 1;
    m_last_signaled_retirement_value = 0;
    m_last_completed_retirement_value = 0;
    m_installed_frame = 0;
    m_installed_device_reset_generation = 0;
    m_installed_mode_token = 0;
    m_last_confirmed_post_overlay_layer = 0;
    m_last_confirmed_post_overlay_state = 0;
    m_last_confirmed_post_overlay_frame = 0;
    m_last_confirmed_post_overlay_valid = false;
    m_installed_overlay = nullptr;
    m_downstream_use_seen = false;
    m_bridge_writer_uncertain = false;
    m_device_removed = false;
}

void RE4XeSSOutputHandoff::preserve_quarantined_generation() noexcept {
    std::lock_guard lock{ m_retirement_mutex };
    m_hard_quarantined = true;
    m_retirement_requested = true;
    m_retirement_status = RetirementStatus::Quarantined;
    auto* retained = new (std::nothrow) RetainedOutputGeneration{};
    if (retained != nullptr) {
        retained->handoff_state = std::move(m_handoff_state);
        retained->saved_original_state = std::move(m_saved_original_state);
        retained->resource = std::move(m_resource_pin);
        retained->device = std::move(m_device);
        retained->queue = std::move(m_queue);
        retained->fence = std::move(m_retirement_fence);
        return;
    }

    if (m_handoff_state != nullptr) {
        m_handoff_state->add_ref();
        m_handoff_state.reset();
    }
    if (m_saved_original_state != nullptr) {
        m_saved_original_state->add_ref();
        m_saved_original_state.reset();
    }
    (void)m_resource_pin.Detach();
    (void)m_device.Detach();
    (void)m_queue.Detach();
    (void)m_retirement_fence.Detach();
}

bool RE4XeSSOutputHandoff::confirmed_device_removal(bool bridge_device_removed) const noexcept {
    if (bridge_device_removed || m_device_removed) {
        return true;
    }
    return m_device != nullptr && FAILED(m_device->GetDeviceRemovedReason());
}
