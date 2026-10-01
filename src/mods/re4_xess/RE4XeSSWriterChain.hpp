#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace re4_xess {

inline constexpr size_t ENGINE_WRITER_CHAIN_CAPACITY = 4;
inline constexpr uint32_t OVERLAY_WRITER_METHOD_RVA = 0x44AF030;

struct EngineWriterWitness {
    uint64_t sequence{};
    uint64_t invocation_id{};
    uintptr_t invocation_stack_pointer{};
    uint64_t clear_pre_sequence{};
    uint64_t clear_post_sequence{};
    uint64_t replacement_pre_sequence{};
    uint64_t replacement_post_sequence{};
    uint64_t current_store_sequence{};
    uint64_t control_generation{};
    uint64_t device_reset_generation{};
    int32_t mode_token{};
    uint32_t writer_thread_id{};
    uintptr_t overlay{};
    uintptr_t slot{};
    uintptr_t cleared_state{};
    uintptr_t clear_state_after{};
    uintptr_t replacement_previous_state{};
    uintptr_t incoming_state{};
    uintptr_t replacement_state_after{};
    uint32_t method_rva{};
    uint32_t clear_pre_write_rva{};
    uint32_t clear_post_write_rva{};
    uint32_t replacement_pre_write_rva{};
    uint32_t replacement_post_write_rva{};
    bool destination_validated{};
    bool same_invocation{};
    bool clear_write_confirmed{};
    bool replacement_write_confirmed{};
    bool re4_image_identity_verified{};
};

struct EngineWriterWitnessChain {
    std::array<EngineWriterWitness, ENGINE_WRITER_CHAIN_CAPACITY> entries{};
    size_t count{};
    uint64_t current_store_sequence{};
    const std::atomic<uint64_t>* live_store_sequence{};
    bool stable{};
    bool overflow{};
};

class EngineWriterWitnessHistory final {
public:
    void reset() noexcept {
        std::lock_guard lock{ m_mutex };
        m_entries = {};
        m_count = 0;
        m_overflow = false;
    }

    bool publish(const EngineWriterWitness& witness) noexcept {
        std::lock_guard lock{ m_mutex };
        if (m_overflow || m_count == m_entries.size()) {
            m_overflow = true;
            return false;
        }
        for (size_t index = 0; index < m_count; ++index) {
            if (m_entries[index].replacement_post_sequence == witness.replacement_post_sequence) {
                m_overflow = true;
                return false;
            }
        }
        size_t insertion = m_count;
        while (insertion > 0 &&
            witness.clear_pre_sequence < m_entries[insertion - 1].clear_pre_sequence) {
            m_entries[insertion] = m_entries[insertion - 1];
            --insertion;
        }
        m_entries[insertion] = witness;
        ++m_count;
        return true;
    }

    EngineWriterWitnessChain snapshot() const noexcept {
        std::lock_guard lock{ m_mutex };
        EngineWriterWitnessChain result{};
        result.entries = m_entries;
        result.count = m_count;
        result.overflow = m_overflow;
        return result;
    }

    void consume_through(uint64_t terminal_store_sequence) noexcept {
        if (terminal_store_sequence == 0) {
            return;
        }
        std::lock_guard lock{ m_mutex };
        size_t retained{};
        for (size_t index = 0; index < m_count; ++index) {
            if (m_entries[index].replacement_post_sequence > terminal_store_sequence) {
                m_entries[retained++] = m_entries[index];
            }
        }
        for (size_t index = retained; index < m_count; ++index) {
            m_entries[index] = {};
        }
        m_count = retained;
    }

private:
    mutable std::mutex m_mutex{};
    std::array<EngineWriterWitness, ENGINE_WRITER_CHAIN_CAPACITY> m_entries{};
    size_t m_count{};
    bool m_overflow{};
};

enum class EngineWriterChainFailure : uint8_t {
    None,
    UnstableSnapshot,
    HistoryOverflow,
    Empty,
    MissingRoot,
    InvalidTransaction,
    IdentityMismatch,
    GenerationMismatch,
    ModeMismatch,
    ThreadMismatch,
    ChainGap,
    StaleOrReplayed,
    FinalStateMismatch,
    StoreSequenceAdvanced,
};

struct EngineWriterChainValidation {
    EngineWriterChainFailure failure{ EngineWriterChainFailure::Empty };
    size_t chain_length{};
    uint64_t first_invocation_id{};
    uint64_t last_invocation_id{};
    uint64_t first_store_sequence{};
    uint64_t terminal_store_sequence{};

    explicit operator bool() const noexcept {
        return failure == EngineWriterChainFailure::None;
    }
};

inline constexpr bool is_valid_engine_writer_transaction(
    const EngineWriterWitness& witness,
    uintptr_t handoff_state,
    uintptr_t saved_original_state) noexcept {
    const bool first_store_path =
        witness.clear_pre_write_rva == 0x44AF161 && witness.clear_post_write_rva == 0x44AF168 &&
        witness.replacement_pre_write_rva == 0x44AF179 && witness.replacement_post_write_rva == 0x44AF180;
    const bool second_store_path =
        witness.clear_pre_write_rva == 0x44AF460 && witness.clear_post_write_rva == 0x44AF467 &&
        witness.replacement_pre_write_rva == 0x44AF478 && witness.replacement_post_write_rva == 0x44AF47F;
    return (first_store_path || second_store_path) &&
        witness.method_rva == OVERLAY_WRITER_METHOD_RVA &&
        witness.destination_validated && witness.same_invocation &&
        witness.clear_write_confirmed && witness.replacement_write_confirmed &&
        witness.re4_image_identity_verified &&
        witness.invocation_id != 0 && witness.invocation_stack_pointer != 0 &&
        witness.writer_thread_id != 0 && witness.overlay != 0 && witness.slot != 0 &&
        witness.clear_pre_sequence != UINT64_MAX &&
        witness.clear_post_sequence == witness.clear_pre_sequence + 1 &&
        witness.replacement_pre_sequence == witness.clear_post_sequence + 1 &&
        witness.replacement_post_sequence == witness.replacement_pre_sequence + 1 &&
        witness.sequence == witness.replacement_post_sequence &&
        witness.current_store_sequence == witness.replacement_post_sequence &&
        handoff_state != 0 && witness.cleared_state == handoff_state &&
        witness.clear_state_after == 0 && witness.replacement_previous_state == 0 &&
        witness.incoming_state != 0 && witness.incoming_state != handoff_state &&
        witness.incoming_state != saved_original_state &&
        witness.replacement_state_after == witness.incoming_state;
}

inline EngineWriterChainValidation validate_engine_writer_chain(
    const EngineWriterWitnessChain& snapshot,
    uintptr_t handoff_state,
    uintptr_t saved_original_state,
    uintptr_t expected_overlay,
    uintptr_t expected_slot,
    uintptr_t observed_state,
    uint64_t expected_control_generation,
    uint64_t installed_control_generation,
    uint64_t expected_device_reset_generation,
    uint64_t installed_device_reset_generation,
    int32_t expected_mode_token,
    uint64_t last_consumed_store_sequence) noexcept {
    EngineWriterChainValidation result{};
    if (!snapshot.stable) {
        result.failure = EngineWriterChainFailure::UnstableSnapshot;
        return result;
    }
    if (snapshot.overflow || snapshot.count > ENGINE_WRITER_CHAIN_CAPACITY) {
        result.failure = EngineWriterChainFailure::HistoryOverflow;
        return result;
    }
    if (snapshot.count == 0) {
        result.failure = EngineWriterChainFailure::Empty;
        return result;
    }

    std::array<EngineWriterWitness, ENGINE_WRITER_CHAIN_CAPACITY> ordered = snapshot.entries;
    for (size_t index = 0; index < snapshot.count; ++index) {
        for (size_t next = index + 1; next < snapshot.count; ++next) {
            if (ordered[next].clear_pre_sequence < ordered[index].clear_pre_sequence) {
                const auto temporary = ordered[index];
                ordered[index] = ordered[next];
                ordered[next] = temporary;
            }
        }
    }

    size_t root = snapshot.count;
    for (size_t index = 0; index < snapshot.count; ++index) {
        if (ordered[index].cleared_state == handoff_state) {
            if (root != snapshot.count) {
                result.failure = EngineWriterChainFailure::InvalidTransaction;
                return result;
            }
            root = index;
        }
    }
    if (root == snapshot.count) {
        result.failure = EngineWriterChainFailure::MissingRoot;
        return result;
    }

    const auto& first = ordered[root];
    if (first.overlay != expected_overlay || first.slot != expected_slot) {
        result.failure = EngineWriterChainFailure::IdentityMismatch;
        return result;
    }
    if (first.control_generation != expected_control_generation ||
        first.control_generation <= installed_control_generation ||
        first.device_reset_generation != expected_device_reset_generation ||
        first.device_reset_generation != installed_device_reset_generation) {
        result.failure = EngineWriterChainFailure::GenerationMismatch;
        return result;
    }
    if (first.mode_token != expected_mode_token) {
        result.failure = EngineWriterChainFailure::ModeMismatch;
        return result;
    }
    if (first.writer_thread_id == 0) {
        result.failure = EngineWriterChainFailure::ThreadMismatch;
        return result;
    }
    if (first.clear_pre_sequence <= last_consumed_store_sequence) {
        result.failure = EngineWriterChainFailure::StaleOrReplayed;
        return result;
    }

    uintptr_t expected_cleared_state = handoff_state;
    uint64_t previous_post_sequence{};
    uint64_t previous_invocation_id{};
    for (size_t index = root; index < snapshot.count; ++index) {
        const auto& witness = ordered[index];
        if (!is_valid_engine_writer_transaction(witness, witness.cleared_state, saved_original_state)) {
            result.failure = EngineWriterChainFailure::InvalidTransaction;
            return result;
        }
        if (witness.overlay != expected_overlay || witness.slot != expected_slot) {
            result.failure = EngineWriterChainFailure::IdentityMismatch;
            return result;
        }
        if (witness.control_generation != expected_control_generation ||
            witness.device_reset_generation != expected_device_reset_generation ||
            witness.device_reset_generation != installed_device_reset_generation) {
            result.failure = EngineWriterChainFailure::GenerationMismatch;
            return result;
        }
        if (witness.mode_token != expected_mode_token) {
            result.failure = EngineWriterChainFailure::ModeMismatch;
            return result;
        }
        if (witness.writer_thread_id != first.writer_thread_id) {
            result.failure = EngineWriterChainFailure::ThreadMismatch;
            return result;
        }
        if (witness.cleared_state != expected_cleared_state ||
            (previous_post_sequence != 0 &&
                (previous_post_sequence == UINT64_MAX ||
                    witness.clear_pre_sequence != previous_post_sequence + 1)) ||
            witness.invocation_id == previous_invocation_id) {
            result.failure = EngineWriterChainFailure::ChainGap;
            return result;
        }
        if (witness.clear_pre_sequence <= last_consumed_store_sequence) {
            result.failure = EngineWriterChainFailure::StaleOrReplayed;
            return result;
        }
        expected_cleared_state = witness.incoming_state;
        previous_post_sequence = witness.replacement_post_sequence;
        previous_invocation_id = witness.invocation_id;
    }

    const auto& terminal = ordered[snapshot.count - 1];
    result.chain_length = snapshot.count - root;
    result.first_invocation_id = first.invocation_id;
    result.last_invocation_id = terminal.invocation_id;
    result.first_store_sequence = first.clear_pre_sequence;
    result.terminal_store_sequence = terminal.replacement_post_sequence;
    if (terminal.replacement_post_sequence != snapshot.current_store_sequence) {
        result.failure = EngineWriterChainFailure::StoreSequenceAdvanced;
        return result;
    }
    if (observed_state != terminal.incoming_state || terminal.replacement_state_after != observed_state) {
        result.failure = EngineWriterChainFailure::FinalStateMismatch;
        return result;
    }
    if (snapshot.live_store_sequence == nullptr ||
        snapshot.live_store_sequence->load(std::memory_order_acquire) != snapshot.current_store_sequence) {
        result.failure = EngineWriterChainFailure::StoreSequenceAdvanced;
        return result;
    }

    result.failure = EngineWriterChainFailure::None;
    return result;
}

} // namespace re4_xess
