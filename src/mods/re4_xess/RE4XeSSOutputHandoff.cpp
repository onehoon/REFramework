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
constexpr uint32_t TRANSITION_PROVENANCE_EVENT_BUDGET = 32;
constexpr uint32_t TRANSITION_PROVENANCE_WINDOW_BUDGET = 16;
constexpr uintptr_t RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET = 0x90;
std::atomic<uint64_t> transition_provenance_sequence{};

constexpr bool is_valid_engine_writer_transaction(
    uint32_t clear_pre_write_rva,
    uint32_t clear_post_write_rva,
    uint32_t replacement_pre_write_rva,
    uint32_t replacement_post_write_rva,
    uintptr_t cleared_state,
    uintptr_t clear_state_after,
    uintptr_t replacement_previous_state,
    uintptr_t incoming_state,
    uintptr_t replacement_state_after,
    uint64_t invocation_id,
    uintptr_t invocation_stack_pointer,
    uint64_t clear_pre_sequence,
    uint64_t clear_post_sequence,
    uint64_t replacement_pre_sequence,
    uint64_t replacement_post_sequence,
    uint64_t current_store_sequence,
    uintptr_t handoff_state,
    uintptr_t saved_original_state,
    bool same_invocation,
    bool clear_write_confirmed,
    bool replacement_write_confirmed) noexcept {
    const bool first_store_path =
        clear_pre_write_rva == 0x44AF161 && clear_post_write_rva == 0x44AF168 &&
        replacement_pre_write_rva == 0x44AF179 && replacement_post_write_rva == 0x44AF180;
    const bool second_store_path =
        clear_pre_write_rva == 0x44AF460 && clear_post_write_rva == 0x44AF467 &&
        replacement_pre_write_rva == 0x44AF478 && replacement_post_write_rva == 0x44AF47F;
    return (first_store_path || second_store_path) && same_invocation &&
        clear_write_confirmed && replacement_write_confirmed &&
        invocation_id != 0 && invocation_stack_pointer != 0 &&
        clear_pre_sequence < clear_post_sequence &&
        clear_post_sequence < replacement_pre_sequence &&
        replacement_pre_sequence < replacement_post_sequence &&
        replacement_post_sequence == current_store_sequence &&
        handoff_state != 0 && cleared_state == handoff_state &&
        clear_state_after == 0 && replacement_previous_state == 0 &&
        incoming_state != 0 && incoming_state != handoff_state &&
        incoming_state != saved_original_state && replacement_state_after == incoming_state;
}

static_assert(is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(is_valid_engine_writer_transaction(
    0x44AF460, 0x44AF467, 0x44AF478, 0x44AF47F,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0, 0, 0x100, 0, 0, 0, 0, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, false));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, false, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0, 0, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x200, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF478, 0x44AF47F,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0x300, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x301, 1, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x300, 0, 0xABC0, 1, 2, 3, 4, 4, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 4, 3, 3, 0x100, 0x200, true, true, true));
static_assert(!is_valid_engine_writer_transaction(
    0x44AF161, 0x44AF168, 0x44AF179, 0x44AF180,
    0x100, 0, 0, 0x300, 0x300, 1, 0xABC0, 1, 2, 3, 4, 5, 0x100, 0x200, true, true, true));

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

const char* engine_writer_chain_failure_name(re4_xess::EngineWriterChainFailure failure) noexcept {
    using Failure = re4_xess::EngineWriterChainFailure;
    switch (failure) {
    case Failure::None: return "none";
    case Failure::UnstableSnapshot: return "unstable-snapshot";
    case Failure::HistoryOverflow: return "history-overflow";
    case Failure::Empty: return "empty";
    case Failure::MissingRoot: return "missing-handoff-root";
    case Failure::InvalidTransaction: return "invalid-transaction";
    case Failure::IdentityMismatch: return "overlay-or-slot-mismatch";
    case Failure::GenerationMismatch: return "generation-mismatch";
    case Failure::ModeMismatch: return "mode-mismatch";
    case Failure::ThreadMismatch: return "writer-thread-mismatch";
    case Failure::ChainGap: return "non-contiguous-chain";
    case Failure::StaleOrReplayed: return "stale-or-replayed";
    case Failure::FinalStateMismatch: return "terminal-state-mismatch";
    case Failure::StoreSequenceAdvanced: return "store-sequence-advanced";
    }
    return "unknown";
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

bool RE4XeSSOutputHandoff::is_verified_engine_replacement_locked(
    const re4_xess::EngineWriterWitnessChain& witness_chain,
    uintptr_t layer_identity,
    uintptr_t slot_identity,
    uintptr_t observed_state,
    const ObservationContext& observation,
    re4_xess::EngineWriterChainValidation* validation_out) const noexcept {
    if (!m_installed || m_installed_overlay != reinterpret_cast<sdk::renderer::layer::Overlay*>(layer_identity) ||
        m_hard_quarantined || m_bridge_writer_uncertain || m_device_removed ||
        m_identity_mismatch_latched.load(std::memory_order_acquire) ||
        m_signature.template_state != reinterpret_cast<uintptr_t>(m_saved_original_state.get())) {
        return false;
    }

    const auto validation = re4_xess::validate_engine_writer_chain(
        witness_chain,
        reinterpret_cast<uintptr_t>(m_handoff_state.get()),
        reinterpret_cast<uintptr_t>(m_saved_original_state.get()),
        layer_identity,
        slot_identity,
        observed_state,
        observation.control_generation,
        m_signature.control_generation,
        observation.device_reset_generation,
        m_installed_device_reset_generation,
        observation.requested_mode_token,
        m_last_consumed_engine_writer_sequence);
    if (validation_out != nullptr) {
        *validation_out = validation;
    }
    return static_cast<bool>(validation);
}

bool RE4XeSSOutputHandoff::restore(
    sdk::renderer::layer::Overlay* layer,
    const ObservationContext& observation,
    const re4_xess::EngineWriterWitnessChain& writer_witness_chain,
    std::string& error,
    uint64_t& consumed_writer_terminal_sequence) {
    error.clear();
    consumed_writer_terminal_sequence = 0;
    const re4_xess::EngineWriterWitness no_writer_witness{};
    const auto& writer_witness = writer_witness_chain.count > 0
        ? writer_witness_chain.entries[writer_witness_chain.count - 1]
        : no_writer_witness;
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
        bool engine_replacement_accepted{};
        uint64_t accepted_installed_generation{};
        bool accepted_marker_pending{};
        uint64_t accepted_signaled_fence_value{};
        uint64_t accepted_trace_id{};
        uint64_t accepted_install_id{};
        uint64_t accepted_frame{};
        uint64_t accepted_install_present{};
        uint64_t accepted_output_generation{};
        uintptr_t accepted_output{};
        uintptr_t accepted_handoff_state{};
        re4_xess::EngineWriterChainValidation writer_validation{};
        {
            std::lock_guard lock{ m_retirement_mutex };
            engine_replacement_accepted = is_verified_engine_replacement_locked(
                writer_witness_chain,
                reinterpret_cast<uintptr_t>(layer),
                reinterpret_cast<uintptr_t>(&main_state),
                reinterpret_cast<uintptr_t>(observed_target_state),
                observation,
                &writer_validation);
            if (engine_replacement_accepted) {
                const bool chain_still_current =
                    reinterpret_cast<uintptr_t>(main_state.get()) == reinterpret_cast<uintptr_t>(observed_target_state) &&
                    writer_witness_chain.live_store_sequence != nullptr &&
                    writer_witness_chain.live_store_sequence->load(std::memory_order_acquire) ==
                        writer_witness_chain.current_store_sequence;
                engine_replacement_accepted = chain_still_current;
                if (!chain_still_current) {
                    writer_validation.failure = re4_xess::EngineWriterChainFailure::StoreSequenceAdvanced;
                }
            }
            if (engine_replacement_accepted) {
                accepted_installed_generation = m_signature.control_generation;
                // A terminal store sequence is single-use and consumes the whole accepted chain.
                m_last_consumed_engine_writer_sequence = writer_validation.terminal_store_sequence;
                consumed_writer_terminal_sequence = writer_validation.terminal_store_sequence;
                m_installed = false;
                m_installed_snapshot.store(false, std::memory_order_release);
                m_installed_overlay = nullptr;
                m_retirement_requested = true;
                m_missing_marker = m_marker_pending;
                m_retirement_status = m_marker_pending
                    ? RetirementStatus::MissingMarker
                    : RetirementStatus::Draining;
                m_failure_reason = "Verified RE4 mode-transition writer replaced Overlay main TargetState; preserving the engine value and retiring the displaced output generation";
                accepted_marker_pending = m_marker_pending;
                accepted_signaled_fence_value = m_last_signaled_retirement_value;
                accepted_trace_id = m_installed_trace_id;
                accepted_install_id = m_installed_install_id;
                accepted_frame = m_installed_frame;
                accepted_install_present = m_installed_present_ordinal;
                accepted_output_generation = m_output_generation_id;
                accepted_output = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
                accepted_handoff_state = reinterpret_cast<uintptr_t>(m_handoff_state.get());
            }
        }
        if (engine_replacement_accepted) {
            const auto& writer_witness = writer_witness_chain.entries[writer_witness_chain.count - 1];
            spdlog::info(
                "[RE4XeSS][EngineTargetReplacement] accepted chainLength={} firstInvocationId={} lastInvocationId={} firstStoreSequence={} terminalStoreSequence={} controlGeneration={} installedGeneration={} modeToken={} deviceResetGeneration={} writerThread={} callbackThread={} methodRva=0x{:x} writerInvocationId={} writerFrameRsp=0x{:x} clearSequence={}->{} replacementSequence={}->{} currentStoreSequence={} clearRva=0x{:x}->0x{:x} replacementRva=0x{:x}->0x{:x} overlay=0x{:x} slot=0x{:x} cleared=0x{:x}->0x{:x} replacement=0x{:x}->0x{:x} markerPending={} lastSignaled={} action=leave-engine-value-in-slot; retire-displaced-handoff-through-existing-fence-gates",
                writer_validation.chain_length,
                static_cast<unsigned long long>(writer_validation.first_invocation_id),
                static_cast<unsigned long long>(writer_validation.last_invocation_id),
                static_cast<unsigned long long>(writer_validation.first_store_sequence),
                static_cast<unsigned long long>(writer_validation.terminal_store_sequence),
                static_cast<unsigned long long>(observation.control_generation),
                static_cast<unsigned long long>(accepted_installed_generation),
                observation.requested_mode_token,
                static_cast<unsigned long long>(observation.device_reset_generation),
                writer_witness.writer_thread_id,
                observation.callback_thread_id,
                writer_witness.method_rva,
                static_cast<unsigned long long>(writer_witness.invocation_id),
                writer_witness.invocation_stack_pointer,
                static_cast<unsigned long long>(writer_witness.clear_pre_sequence),
                static_cast<unsigned long long>(writer_witness.clear_post_sequence),
                static_cast<unsigned long long>(writer_witness.replacement_pre_sequence),
                static_cast<unsigned long long>(writer_witness.replacement_post_sequence),
                static_cast<unsigned long long>(writer_witness.current_store_sequence),
                writer_witness.clear_pre_write_rva,
                writer_witness.clear_post_write_rva,
                writer_witness.replacement_pre_write_rva,
                writer_witness.replacement_post_write_rva,
                writer_witness.overlay,
                writer_witness.slot,
                writer_witness.cleared_state,
                writer_witness.clear_state_after,
                writer_witness.replacement_previous_state,
                writer_witness.incoming_state,
                accepted_marker_pending,
                static_cast<unsigned long long>(accepted_signaled_fence_value));
            const auto divergence_sequence = find_matching_provenance_divergence(
                reinterpret_cast<uintptr_t>(layer),
                reinterpret_cast<uintptr_t>(&main_state),
                reinterpret_cast<uintptr_t>(observed_target_state),
                accepted_handoff_state,
                observation.control_generation,
                observation.device_reset_generation);
            if (divergence_sequence != 0) {
                spdlog::info(
                    "[RE4XeSS][HandoffProvenance] divergence-resolved seq={} resolution=accepted-verified-engine-writer-chain chainLength={} firstInvocationId={} terminalStoreSequence={} controlGeneration={} deviceResetGeneration={} overlay=0x{:x} slot=0x{:x} current=0x{:x} expectedHandoff=0x{:x}",
                    static_cast<unsigned long long>(divergence_sequence),
                    writer_validation.chain_length,
                    static_cast<unsigned long long>(writer_validation.first_invocation_id),
                    static_cast<unsigned long long>(writer_validation.terminal_store_sequence),
                    static_cast<unsigned long long>(observation.control_generation),
                    static_cast<unsigned long long>(observation.device_reset_generation),
                    reinterpret_cast<uintptr_t>(layer),
                    reinterpret_cast<uintptr_t>(&main_state),
                    reinterpret_cast<uintptr_t>(observed_target_state),
                    accepted_handoff_state);
            }
            RE4XeSSLifetimeTrace::Event trace_event{};
            trace_event.kind = RE4XeSSLifetimeTrace::Kind::OutputRestore;
            trace_event.trace_id = accepted_trace_id;
            trace_event.install_id = accepted_install_id;
            trace_event.frame_id = accepted_frame;
            trace_event.frame_valid = true;
            trace_event.callback_ordinal = observation.callback_ordinal;
            trace_event.present_ordinal = observation.present_ordinal;
            trace_event.related_present_ordinal = accepted_install_present;
            trace_event.output_generation = accepted_output_generation;
            trace_event.control_generation = observation.control_generation;
            trace_event.device_reset_generation = observation.device_reset_generation;
            trace_event.writer_sequence = writer_witness.sequence;
            trace_event.writer_invocation_id = writer_witness.invocation_id;
            trace_event.writer_clear_pre_sequence = writer_witness.clear_pre_sequence;
            trace_event.writer_clear_post_sequence = writer_witness.clear_post_sequence;
            trace_event.writer_replacement_pre_sequence = writer_witness.replacement_pre_sequence;
            trace_event.writer_replacement_post_sequence = writer_witness.replacement_post_sequence;
            trace_event.writer_method_rva = writer_witness.method_rva;
            trace_event.writer_transaction_confirmed = true;
            trace_event.downstream_fence_value = accepted_signaled_fence_value;
            trace_event.output_resource = accepted_output;
            trace_event.target_state = reinterpret_cast<uintptr_t>(observed_target_state);
            trace_event.overlay = reinterpret_cast<uintptr_t>(layer);
            trace_event.swapchain = observation.swapchain;
            trace_event.device = observation.device;
            trace_event.queue = observation.queue;
            trace_event.thread_id = observation.callback_thread_id;
            trace_event.present_valid = observation.present_ordinal_valid;
            trace_event.mapping_ambiguous = true;
            RE4XeSSLifetimeTrace::set_reason(trace_event, "verified-engine-replacement");
            RE4XeSSLifetimeTrace::instance().record(trace_event);
            log_transition_provenance(
                "engine-target-replacement-accepted", observation,
                reinterpret_cast<uintptr_t>(layer),
                reinterpret_cast<uintptr_t>(observed_target_state), true, 1u << 8);
            return true;
        }

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
        uint64_t mismatch_trace_id{};
        uint64_t mismatch_install_id{};
        uint64_t mismatch_install_present{};
        uint64_t mismatch_output_generation{};
        uintptr_t mismatch_output{};
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
            mismatch_trace_id = m_installed_trace_id;
            mismatch_install_id = m_installed_install_id;
            mismatch_install_present = m_installed_present_ordinal;
            mismatch_output_generation = m_output_generation_id;
            mismatch_output = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
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
                "[RE4XeSS][OutputHandoffMismatch] phase=pre-overlay-restore tid={} frameKnown={} frame={} requestedModeToken={} controlGeneration={} deviceResetGeneration={} installedModeToken={} installedControlGeneration={} installedDeviceResetGeneration={} installedFrame={} currentLayer=0x{:x} installedLayer=0x{:x} targetStateSlot=0x{:x} targetStateSlotOffset=0x{:x} sameLayerAsLastConfirmedPostOverlay={} observedMainTargetState=0x{:x} observedTargetRelation=third-object expectedHandoffState=0x{:x} savedOriginalState=0x{:x} lastTemplateTargetState=0x{:x} lastConfirmedPostOverlayValid={} lastConfirmedPostOverlayFrame={} lastConfirmedPostOverlayLayer=0x{:x} lastConfirmedPostOverlayTargetState=0x{:x} installed={} markerPending={} retirementRequested={} hardQuarantined={} bridgeWriterUncertain={} writerChainCount={} writerChainOverflow={} writerSnapshotStable={} writerSnapshotStoreSequence={} writerChainValidation={} downstreamFenceLastSignaled={} cachedFenceCompleted={} actualFenceRead={} actualFenceCompleted={} actualFenceResult={}",
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
                writer_witness_chain.count,
                writer_witness_chain.overflow,
                writer_witness_chain.stable,
                static_cast<unsigned long long>(writer_witness_chain.current_store_sequence),
                engine_writer_chain_failure_name(writer_validation.failure),
                static_cast<unsigned long long>(signaled_fence_value),
                static_cast<unsigned long long>(completed_fence_value),
                actual_fence_read,
                static_cast<unsigned long long>(actual_fence_value),
                !actual_fence_read ? "unavailable" :
                    actual_fence_value == std::numeric_limits<uint64_t>::max() ? "device-removed-or-invalid" :
                    actual_fence_value >= signaled_fence_value ? "completed-through-last-signal" : "not-yet-complete");
            RE4XeSSLifetimeTrace::Event trace_event{};
            trace_event.kind = RE4XeSSLifetimeTrace::Kind::OutputRestore;
            trace_event.trace_id = mismatch_trace_id;
            trace_event.install_id = mismatch_install_id;
            trace_event.frame_id = installed_frame;
            trace_event.frame_valid = mismatch_trace_id != 0;
            trace_event.callback_ordinal = observation.callback_ordinal;
            trace_event.present_ordinal = observation.present_ordinal;
            trace_event.related_present_ordinal = mismatch_install_present;
            trace_event.output_generation = mismatch_output_generation;
            trace_event.control_generation = observation.control_generation;
            trace_event.device_reset_generation = observation.device_reset_generation;
            trace_event.writer_sequence = writer_witness.sequence;
            trace_event.writer_invocation_id = writer_witness.invocation_id;
            trace_event.writer_clear_pre_sequence = writer_witness.clear_pre_sequence;
            trace_event.writer_clear_post_sequence = writer_witness.clear_post_sequence;
            trace_event.writer_replacement_pre_sequence = writer_witness.replacement_pre_sequence;
            trace_event.writer_replacement_post_sequence = writer_witness.replacement_post_sequence;
            trace_event.writer_method_rva = writer_witness.method_rva;
            trace_event.writer_transaction_confirmed =
                writer_witness.same_invocation &&
                writer_witness.clear_write_confirmed &&
                writer_witness.replacement_write_confirmed &&
                writer_witness.re4_image_identity_verified;
            trace_event.downstream_fence_value = signaled_fence_value;
            trace_event.actual_completed_value = actual_fence_value;
            trace_event.actual_completed_value_valid =
                actual_fence_read && actual_fence_value != std::numeric_limits<uint64_t>::max();
            trace_event.output_resource = mismatch_output;
            trace_event.target_state = reinterpret_cast<uintptr_t>(observed_target_state);
            trace_event.overlay = reinterpret_cast<uintptr_t>(layer);
            trace_event.swapchain = observation.swapchain;
            trace_event.device = observation.device;
            trace_event.queue = observation.queue;
            trace_event.thread_id = observation.callback_thread_id;
            trace_event.present_valid = observation.present_ordinal_valid;
            trace_event.mapping_ambiguous = true;
            RE4XeSSLifetimeTrace::set_reason(trace_event, "unverified-engine-replacement-quarantined");
            RE4XeSSLifetimeTrace::instance().record(trace_event);
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
    uint64_t restored_trace_id{};
    uint64_t restored_install_id{};
    uint64_t restored_frame{};
    uint64_t restored_install_present{};
    uint64_t restored_output_generation{};
    uint64_t restored_fence_value{};
    uintptr_t restored_output{};
    uintptr_t restored_state{};
    bool restored_marker_pending{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_installed = false;
        m_installed_snapshot.store(false, std::memory_order_release);
        m_installed_overlay = nullptr;
        restored_trace_id = m_installed_trace_id;
        restored_install_id = m_installed_install_id;
        restored_frame = m_installed_frame;
        restored_install_present = m_installed_present_ordinal;
        restored_output_generation = m_output_generation_id;
        restored_fence_value = m_last_signaled_retirement_value;
        restored_output = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
        restored_state = reinterpret_cast<uintptr_t>(m_saved_original_state.get());
        restored_marker_pending = m_marker_pending;
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
    if (RE4XeSSLifetimeTrace::instance().enabled()) {
        RE4XeSSLifetimeTrace::Event restore_event{};
        restore_event.kind = RE4XeSSLifetimeTrace::Kind::OutputRestore;
        restore_event.trace_id = restored_trace_id;
        restore_event.install_id = restored_install_id;
        restore_event.frame_id = restored_frame;
        restore_event.frame_valid = true;
        restore_event.callback_ordinal = observation.callback_ordinal;
        restore_event.present_ordinal = observation.present_ordinal;
        restore_event.related_present_ordinal = restored_install_present;
        restore_event.output_generation = restored_output_generation;
        restore_event.control_generation = observation.control_generation;
        restore_event.device_reset_generation = observation.device_reset_generation;
        restore_event.downstream_fence_value = restored_fence_value;
        restore_event.output_resource = restored_output;
        restore_event.target_state = restored_state;
        restore_event.overlay = reinterpret_cast<uintptr_t>(layer);
        restore_event.swapchain = observation.swapchain;
        restore_event.device = observation.device;
        restore_event.queue = observation.queue;
        restore_event.thread_id = observation.callback_thread_id;
        restore_event.present_valid = observation.present_ordinal_valid;
        restore_event.mapping_ambiguous = true;
        RE4XeSSLifetimeTrace::set_reason(restore_event,
            restored_marker_pending ? "restore-marker-pending" : "restore-original");
        RE4XeSSLifetimeTrace::instance().record(restore_event);
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
    m_provenance_window_budget.store(TRANSITION_PROVENANCE_WINDOW_BUDGET, std::memory_order_release);
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
        "[RE4XeSS][HandoffProvenance] armed={} reason={} imageIdentity={} imageBase=0x{:x} imageSize=0x{:x} imageChecksum=0x{:x} activation=DebugLog+RE4XeSS_HandoffProvenance writerInstrumentation={}",
        enabled,
        reason,
        image_size == 0x0E405000 && image_checksum == 0x0DEE3479 ? "RE4-1.5.9.0-verified" : "unverified",
        image_base,
        image_size,
        image_checksum,
        enabled ? "deferred-until-live-overlay-anchor" : "disabled");
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
    auto window_remaining = m_provenance_window_budget.load(std::memory_order_acquire);
    while (window_remaining != 0 && !m_provenance_window_budget.compare_exchange_weak(
        window_remaining, window_remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    const bool window_sample = window_remaining != 0;
    if (!first_phase && !window_sample) {
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
    const auto sampled_layer = layer_identity != 0 ? layer_identity : installed_layer;
    if (sampled_layer != 0 && sampled_layer <= UINTPTR_MAX - RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET) {
        target_state_slot = sampled_layer + RE4_OVERLAY_MAIN_TARGET_STATE_OFFSET;
        if (current_state_known && layer_identity != 0) {
            auto* const layer = reinterpret_cast<sdk::renderer::layer::Overlay*>(layer_identity);
            if (reinterpret_cast<uintptr_t>(&layer->get_main_target_state()) == target_state_slot) {
                m_provenance_validated_layer.store(sampled_layer, std::memory_order_release);
            }
        }
        if (m_provenance_validated_layer.load(std::memory_order_acquire) == sampled_layer) {
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
    bool first_divergence = slot_read_valid && installed && sampled_layer == installed_layer &&
        observed_current_state != handoff_state &&
        observed_current_state != saved_state;
    if (!slot_read_valid) {
        previous_current_state = m_provenance_last_current_state.load(std::memory_order_acquire);
    }

    const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    ProvenanceSample sample{
        phase,
        transition_provenance_sequence.fetch_add(1, std::memory_order_relaxed) + 1,
        timestamp_us,
        observation.control_generation,
        observation.device_reset_generation,
        observation.frame_id,
        observation.callback_thread_id,
        observation.requested_mode_token,
        installed_mode_token,
        sampled_layer,
        target_state_slot,
        slot_read_valid ? observed_current_state : current_state_identity,
        handoff_state,
        saved_state,
        observation.frame_id_valid,
        slot_read_valid,
        false,
        identity_changed,
        marker_pending,
        installed,
        retirement_requested,
        hard_quarantined,
    };
    std::array<ProvenanceSample, 16> preceding{};
    size_t preceding_count{};
    {
        std::lock_guard lock{ m_provenance_mutex };
        if (first_divergence) {
            if (m_provenance_divergence_count == 0) {
                ++m_provenance_divergence_count;
            } else {
                first_divergence = false;
            }
        }
        sample.first_divergence = first_divergence;
        if (slot_read_valid) {
            m_provenance_ring[m_provenance_ring_next] = sample;
            m_provenance_ring_next = (m_provenance_ring_next + 1) % m_provenance_ring.size();
            m_provenance_ring_size = std::min(m_provenance_ring_size + 1, m_provenance_ring.size());
        }
        if (first_divergence) {
            preceding_count = m_provenance_ring_size;
            const auto first = (m_provenance_ring_next + m_provenance_ring.size() - preceding_count) % m_provenance_ring.size();
            for (size_t i = 0; i < preceding_count; ++i) {
                preceding[i] = m_provenance_ring[(first + i) % m_provenance_ring.size()];
            }
        }
    }
    if (!first_phase && !window_sample && !first_divergence) {
        return;
    }
    auto remaining = m_provenance_event_budget.load(std::memory_order_acquire);
    while (remaining != 0 && !m_provenance_event_budget.compare_exchange_weak(
        remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if (remaining == 0 && !first_divergence) {
        return;
    }
    {
        std::lock_guard lock{ m_provenance_mutex };
        ++m_provenance_event_count;
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
        sampled_layer,
        installed_layer,
        target_state_slot,
        target_state_slot != 0 ? target_state_slot - sampled_layer : 0,
        current_state_known || slot_read_valid,
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
        spdlog::warn("[RE4XeSS][HandoffProvenance] first-divergence-pending seq={} current=0x{:x} expected=0x{:x} saved=0x{:x} precedingSamples={} resolution=awaiting-validated-writer-chain",
            static_cast<unsigned long long>(sample.sequence), observed_current_state, handoff_state, saved_state, preceding_count);
        for (size_t i = 0; i < preceding_count; ++i) {
            const auto& prior = preceding[i];
            spdlog::info("[RE4XeSS][HandoffProvenanceSample] seq={} timestampUs={} phase={} tid={} frameKnown={} frame={} controlGeneration={} deviceResetGeneration={} mode={} overlay=0x{:x} slot=0x{:x} slotReadValid={} current=0x{:x} expected=0x{:x} saved=0x{:x} markerPending={} installed={} retirementRequested={} hardQuarantined={} identityChanged={}",
                static_cast<unsigned long long>(prior.sequence), prior.timestamp_us, prior.phase, prior.thread_id,
                prior.frame_id_valid, static_cast<unsigned long long>(prior.frame_id),
                static_cast<unsigned long long>(prior.control_generation),
                static_cast<unsigned long long>(prior.device_reset_generation), prior.requested_mode_token,
                prior.overlay, prior.slot, prior.slot_read_valid, prior.current_state,
                prior.expected_state, prior.saved_state, prior.marker_pending, prior.installed,
                prior.retirement_requested, prior.hard_quarantined, prior.identity_changed);
        }
    }
}

uint64_t RE4XeSSOutputHandoff::find_matching_provenance_divergence(
    uintptr_t layer_identity,
    uintptr_t slot_identity,
    uintptr_t current_state_identity,
    uintptr_t expected_state_identity,
    uint64_t control_generation,
    uint64_t device_reset_generation) noexcept {
    std::lock_guard lock{ m_provenance_mutex };
    for (size_t offset = 0; offset < m_provenance_ring_size; ++offset) {
        const auto index = (m_provenance_ring_next + m_provenance_ring.size() - 1 - offset) %
            m_provenance_ring.size();
        const auto& sample = m_provenance_ring[index];
        if (sample.first_divergence && sample.slot_read_valid &&
            sample.overlay == layer_identity && sample.slot == slot_identity &&
            sample.current_state == current_state_identity &&
            sample.expected_state == expected_state_identity &&
            sample.control_generation == control_generation &&
            sample.device_reset_generation == device_reset_generation) {
            return sample.sequence;
        }
    }
    return 0;
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
    m_output_generation_id = ++m_next_output_generation;
    m_expected_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_retirement_value = 1;
    m_last_signaled_retirement_value = 0;
    m_last_completed_retirement_value = 0;
    m_installed_frame = 0;
    m_installed_install_id = 0;
    m_installed_output_use_token = 0;
    m_installed_trace_id = 0;
    m_installed_present_ordinal = 0;
    m_installed_submit_ordinal = 0;
    m_installed_writer_fence_value = 0;
    m_installed_bridge_slot = 0;
    m_trace_last_observed_completed_value = 0;
    m_trace_marker_count = 0;
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
    const bool trace_enabled = RE4XeSSLifetimeTrace::instance().enabled();
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
    m_installed_install_id = ++m_next_install_id;
    m_installed_output_use_token = trace_enabled ? ++m_next_output_use_token : 0;
    m_installed_trace_id = observation.trace_id;
    m_installed_present_ordinal = observation.present_ordinal_valid ? observation.present_ordinal : 0;
    m_installed_submit_ordinal = observation.submit_ordinal;
    m_installed_writer_fence_value = observation.writer_fence_value;
    m_installed_bridge_slot = observation.bridge_slot;
    m_installed_mode_token = observation.requested_mode_token;
    m_installed_device_reset_generation = observation.device_reset_generation;
    m_downstream_use_seen = true;
    m_marker_pending = true;
    m_missing_marker = false;
    m_installed_snapshot.store(true, std::memory_order_release);
    m_retirement_status = RetirementStatus::Active;
    m_failure_reason.clear();
    const auto installed_state_identity = reinterpret_cast<uintptr_t>(m_handoff_state.get());
    const auto output_identity = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
    const auto output_generation = m_output_generation_id;
    const auto install_id = m_installed_install_id;
    const auto output_use_token = m_installed_output_use_token;
    const auto install_present_ordinal = m_installed_present_ordinal;

    if (trace_enabled) {
        RE4XeSSLifetimeTrace::Event trace_event{};
        trace_event.kind = RE4XeSSLifetimeTrace::Kind::OutputInstall;
        trace_event.trace_id = observation.trace_id;
        trace_event.install_id = install_id;
        trace_event.output_use_token = output_use_token;
        trace_event.consumer_evidence = RE4XeSSLifetimeTrace::ConsumerEvidence::ReaderNotObserved;
        trace_event.frame_id = observation.frame_id;
        trace_event.frame_valid = observation.frame_id_valid;
        trace_event.callback_ordinal = observation.callback_ordinal;
        trace_event.submit_ordinal = observation.submit_ordinal;
        trace_event.present_ordinal = install_present_ordinal;
        trace_event.output_generation = output_generation;
        trace_event.control_generation = observation.control_generation;
        trace_event.device_reset_generation = observation.device_reset_generation;
        trace_event.writer_fence_value = observation.writer_fence_value;
        trace_event.output_resource = output_identity;
        trace_event.target_state = installed_state_identity;
        trace_event.overlay = reinterpret_cast<uintptr_t>(layer);
        trace_event.swapchain = observation.swapchain;
        trace_event.device = observation.device;
        trace_event.queue = observation.queue;
        trace_event.command_queue_type = observation.command_queue_type;
        trace_event.command_queue_type_valid = observation.command_queue_type_valid;
        trace_event.thread_id = observation.callback_thread_id;
        trace_event.bridge_slot = observation.bridge_slot;
        trace_event.api_succeeded = true;
        trace_event.queue_submitted = true;
        trace_event.writer_signal_succeeded = true;
        trace_event.mapping_ambiguous = true;
        trace_event.mapping_observed = true;
        trace_event.mapping_state = observation.present_ordinal_valid
            ? RE4XeSSLifetimeTrace::MappingState::InferredCandidate
            : RE4XeSSLifetimeTrace::MappingState::Unknown;
        RE4XeSSLifetimeTrace::set_reason(trace_event,
            observation.present_ordinal_valid ? "installed-present-context-only" : "install-present-unknown");
        RE4XeSSLifetimeTrace::instance().record(trace_event);
    }
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
    const auto marker_trace_id = m_installed_trace_id;
    const auto marker_install_id = m_installed_install_id;
    const auto marker_frame = m_installed_frame;
    const auto marker_output_generation = m_output_generation_id;
    const auto marker_install_present = m_installed_present_ordinal;
    const auto marker_output_identity = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
    const auto marker_target_state = reinterpret_cast<uintptr_t>(m_handoff_state.get());
    const auto marker_queue_identity = reinterpret_cast<uintptr_t>(m_queue.Get());
    const auto marker_device_identity = reinterpret_cast<uintptr_t>(m_device.Get());
    bool trace_marker_buffer_full{};
    const auto record_marker = [&](HRESULT result, bool queued, uint64_t fence_value, std::string_view reason) {
        if (!RE4XeSSLifetimeTrace::instance().enabled()) {
            return;
        }
        RE4XeSSLifetimeTrace::Event event{};
        event.kind = RE4XeSSLifetimeTrace::Kind::Marker;
        event.trace_id = marker_trace_id;
        event.install_id = marker_install_id;
        event.frame_id = marker_frame;
        event.frame_valid = true;
        event.present_ordinal = observation.present_ordinal;
        event.related_present_ordinal = marker_install_present;
        event.output_generation = marker_output_generation;
        event.control_generation = observation.control_generation;
        event.device_reset_generation = observation.device_reset_generation;
        event.downstream_fence_value = fence_value;
        event.output_resource = marker_output_identity;
        event.target_state = marker_target_state;
        event.swapchain = observation.swapchain;
        event.device = marker_device_identity;
        event.queue = marker_queue_identity;
        event.command_queue_type = m_queue != nullptr
            ? static_cast<int32_t>(m_queue->GetDesc().Type) : -1;
        event.command_queue_type_valid = m_queue != nullptr;
        event.cached_completed_value = m_last_completed_retirement_value;
        event.cached_completed_value_valid = true;
        event.thread_id = observation.callback_thread_id;
        event.result = result;
        event.present_valid = observation.present_ordinal_valid;
        event.marker_queued = queued;
        // A Present callback ordinal is not proof that a command list read this output.
        event.mapping_ambiguous = true;
        event.mapping_observed = true;
        event.mapping_state = observation.present_ordinal_valid
            ? RE4XeSSLifetimeTrace::MappingState::InferredCandidate
            : RE4XeSSLifetimeTrace::MappingState::Unknown;
        RE4XeSSLifetimeTrace::set_reason(event, reason);
        RE4XeSSLifetimeTrace::instance().record(event);
    };
    if (active_device != m_device.Get() || active_queue != m_queue.Get()) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "post-Present active device/queue does not match the handoff generation";
        const auto failure = m_failure_reason;
        lock.unlock();
        record_marker(E_INVALIDARG, false, 0, "marker-device-or-queue-mismatch");
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
        record_marker(E_FAIL, false, 0, "marker-fence-value-exhausted");
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
        record_marker(result, false, value, "marker-signal-failed");
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
    if (RE4XeSSLifetimeTrace::instance().enabled()) {
        if (m_trace_marker_count < m_trace_markers.size()) {
            m_trace_markers[m_trace_marker_count++] = TraceMarker{
                marker_trace_id,
                marker_install_id,
                marker_frame,
                observation.present_ordinal_valid ? observation.present_ordinal : 0,
                marker_output_generation,
                value,
                marker_output_identity,
                observation.callback_thread_id,
            };
        } else {
            trace_marker_buffer_full = true;
        }
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
    record_marker(S_OK, true, value,
            trace_marker_buffer_full ? "marker-queued-trace-buffer-full" :
            !observation.present_ordinal_valid ? "marker-queued-present-unknown" :
            marker_install_present == observation.present_ordinal ? "marker-queued-same-present-ordinal" :
            "marker-queued-different-present-ordinal");
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

void RE4XeSSOutputHandoff::observe_fence_completion() noexcept {
    auto& trace = RE4XeSSLifetimeTrace::instance();
    if (!trace.enabled()) {
        return;
    }

    std::lock_guard lock{ m_retirement_mutex };
    if (!m_has_generation_snapshot.load(std::memory_order_acquire) || m_retirement_fence == nullptr) {
        return;
    }

    const auto completed = m_retirement_fence->GetCompletedValue();
    const auto cached_completed = m_last_completed_retirement_value;
    if (completed == std::numeric_limits<uint64_t>::max()) {
        RE4XeSSLifetimeTrace::Event event{};
        event.kind = RE4XeSSLifetimeTrace::Kind::DownstreamFenceComplete;
        event.output_generation = m_output_generation_id;
        event.device = reinterpret_cast<uintptr_t>(m_device.Get());
        event.queue = reinterpret_cast<uintptr_t>(m_queue.Get());
        event.thread_id = GetCurrentThreadId();
        event.actual_completed_value = completed;
        event.actual_completed_value_valid = false;
        event.cached_completed_value = cached_completed;
        event.cached_completed_value_valid = true;
        event.command_queue_type = m_queue != nullptr
            ? static_cast<int32_t>(m_queue->GetDesc().Type) : -1;
        event.command_queue_type_valid = m_queue != nullptr;
        event.mapping_ambiguous = true;
        RE4XeSSLifetimeTrace::set_reason(event, "fence-returned-uint64-max-not-completion");
        trace.record(event);
        return;
    }
    if (completed <= m_trace_last_observed_completed_value) {
        return;
    }
    m_trace_last_observed_completed_value = completed;

    while (m_trace_marker_count != 0 && m_trace_markers[0].fence_value <= completed) {
        const auto marker = m_trace_markers[0];
        for (size_t i = 1; i < m_trace_marker_count; ++i) {
            m_trace_markers[i - 1] = m_trace_markers[i];
        }
        --m_trace_marker_count;

        RE4XeSSLifetimeTrace::Event event{};
        event.kind = RE4XeSSLifetimeTrace::Kind::DownstreamFenceComplete;
        event.trace_id = marker.trace_id;
        event.install_id = marker.install_id;
        event.frame_id = marker.frame_id;
        event.frame_valid = true;
        event.present_ordinal = marker.present_ordinal;
        event.output_generation = marker.output_generation;
        event.downstream_fence_value = marker.fence_value;
        event.actual_completed_value = completed;
        event.actual_completed_value_valid = true;
        event.output_resource = marker.output_resource;
        event.device = reinterpret_cast<uintptr_t>(m_device.Get());
        event.queue = reinterpret_cast<uintptr_t>(m_queue.Get());
        event.command_queue_type = m_queue != nullptr
            ? static_cast<int32_t>(m_queue->GetDesc().Type) : -1;
        event.command_queue_type_valid = m_queue != nullptr;
        event.thread_id = GetCurrentThreadId();
        event.marker_queued = true;
        event.cached_completed_value = cached_completed;
        event.cached_completed_value_valid = true;
        event.mapping_ambiguous = true;
        RE4XeSSLifetimeTrace::set_reason(event, "downstream-fence-completion-observed");
        trace.record(event);
    }
}

RE4XeSSOutputHandoff::Snapshot RE4XeSSOutputHandoff::snapshot() const {
    std::lock_guard lock{ m_retirement_mutex };
    return {
        m_has_generation_snapshot.load(std::memory_order_acquire),
        m_installed_snapshot.load(std::memory_order_acquire),
        m_retirement_status,
        m_failure_reason,
        m_output_generation_id,
        m_installed_install_id,
        m_installed_output_use_token,
        m_installed_trace_id,
        m_installed_frame,
        m_installed_present_ordinal,
        m_signature.control_generation,
        m_installed_device_reset_generation,
        m_last_signaled_retirement_value,
        m_last_completed_retirement_value,
        m_installed_mode_token,
        reinterpret_cast<uintptr_t>(m_resource_pin.Get()),
        reinterpret_cast<uintptr_t>(m_handoff_state.get()),
        reinterpret_cast<uintptr_t>(m_installed_overlay),
        m_marker_pending,
    };
}

void RE4XeSSOutputHandoff::observe_overlay(
    sdk::renderer::layer::Overlay* layer,
    const ObservationContext& observation,
    const re4_xess::EngineWriterWitnessChain& writer_witness_chain) {
    if (layer == nullptr) {
        return;
    }
    const re4_xess::EngineWriterWitness no_writer_witness{};
    const auto& writer_witness = writer_witness_chain.count > 0
        ? writer_witness_chain.entries[writer_witness_chain.count - 1]
        : no_writer_witness;
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
    bool verified_engine_replacement{};
    bool log_engine_replacement{};
    re4_xess::EngineWriterChainValidation writer_validation{};
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
    } else {
        std::lock_guard lock{ m_retirement_mutex };
        verified_engine_replacement = is_verified_engine_replacement_locked(
            writer_witness_chain,
            reinterpret_cast<uintptr_t>(layer),
            reinterpret_cast<uintptr_t>(&main_state),
            observed_state,
            observation,
            &writer_validation);
        if (verified_engine_replacement) {
            if (m_engine_replacement_observed_sequence != writer_validation.terminal_store_sequence) {
                m_engine_replacement_observed_sequence = writer_validation.terminal_store_sequence;
                log_engine_replacement = debug_log;
            }
        } else if (debug_log && !m_post_overlay_mismatch_logged) {
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
    } else if (log_engine_replacement) {
        const auto& writer_witness = writer_witness_chain.entries[writer_witness_chain.count - 1];
        spdlog::info("[RE4XeSS][EngineTargetReplacement] observed chainLength={} firstInvocationId={} lastInvocationId={} firstStoreSequence={} terminalStoreSequence={} controlGeneration={} modeToken={} deviceResetGeneration={} writerThread={} observerThread={} methodRva=0x{:x} writerInvocationId={} writerFrameRsp=0x{:x} clearSequence={}->{} replacementSequence={}->{} currentStoreSequence={} clearRva=0x{:x}->0x{:x} replacementRva=0x{:x}->0x{:x} overlay=0x{:x} slot=0x{:x} cleared=0x{:x}->0x{:x} replacement=0x{:x}->0x{:x} action=await-pre-overlay-retirement-reconciliation",
            writer_validation.chain_length,
            static_cast<unsigned long long>(writer_validation.first_invocation_id),
            static_cast<unsigned long long>(writer_validation.last_invocation_id),
            static_cast<unsigned long long>(writer_validation.first_store_sequence),
            static_cast<unsigned long long>(writer_validation.terminal_store_sequence),
            static_cast<unsigned long long>(writer_witness.control_generation),
            writer_witness.mode_token,
            static_cast<unsigned long long>(writer_witness.device_reset_generation),
            writer_witness.writer_thread_id,
            observation.callback_thread_id,
            writer_witness.method_rva,
            static_cast<unsigned long long>(writer_witness.invocation_id),
            writer_witness.invocation_stack_pointer,
            static_cast<unsigned long long>(writer_witness.clear_pre_sequence),
            static_cast<unsigned long long>(writer_witness.clear_post_sequence),
            static_cast<unsigned long long>(writer_witness.replacement_pre_sequence),
            static_cast<unsigned long long>(writer_witness.replacement_post_sequence),
            static_cast<unsigned long long>(writer_witness.current_store_sequence),
            writer_witness.clear_pre_write_rva,
            writer_witness.clear_post_write_rva,
            writer_witness.replacement_pre_write_rva,
            writer_witness.replacement_post_write_rva,
            writer_witness.overlay,
            writer_witness.slot,
            writer_witness.cleared_state,
            writer_witness.clear_state_after,
            writer_witness.replacement_previous_state,
            writer_witness.incoming_state);
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
    m_output_generation_id = 0;
    m_installed_install_id = 0;
    m_installed_output_use_token = 0;
    m_installed_trace_id = 0;
    m_installed_present_ordinal = 0;
    m_installed_submit_ordinal = 0;
    m_installed_writer_fence_value = 0;
    m_installed_bridge_slot = 0;
    m_trace_last_observed_completed_value = 0;
    m_trace_marker_count = 0;
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
