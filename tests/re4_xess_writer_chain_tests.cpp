// Standalone deterministic tests:
// cl /nologo /std:c++23 /EHsc /W4 /I src tests\re4_xess_writer_chain_tests.cpp /Fe:re4_xess_writer_chain_tests.exe

#include "mods/re4_xess/RE4XeSSWriterChain.hpp"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <thread>

using namespace re4_xess;

namespace {

constexpr uintptr_t HANDOFF = 0x1000;
constexpr uintptr_t SAVED = 0x2000;
constexpr uintptr_t OVERLAY = 0x3000;
constexpr uintptr_t SLOT = OVERLAY + 0x90;
constexpr uint64_t CONTROL_GENERATION = 12;
constexpr uint64_t DEVICE_GENERATION = 3;
constexpr int32_t MODE = 5;
constexpr uint32_t WRITER_THREAD = 44;

EngineWriterWitness make_transaction(
    uint64_t invocation,
    uint64_t first_sequence,
    uintptr_t previous_state,
    uintptr_t incoming_state) {
    EngineWriterWitness witness{};
    witness.sequence = first_sequence + 3;
    witness.invocation_id = invocation;
    witness.invocation_stack_pointer = 0x800000 + invocation * 0x100;
    witness.clear_pre_sequence = first_sequence;
    witness.clear_post_sequence = first_sequence + 1;
    witness.replacement_pre_sequence = first_sequence + 2;
    witness.replacement_post_sequence = first_sequence + 3;
    witness.current_store_sequence = first_sequence + 3;
    witness.control_generation = CONTROL_GENERATION;
    witness.device_reset_generation = DEVICE_GENERATION;
    witness.mode_token = MODE;
    witness.writer_thread_id = WRITER_THREAD;
    witness.overlay = OVERLAY;
    witness.slot = SLOT;
    witness.cleared_state = previous_state;
    witness.clear_state_after = 0;
    witness.replacement_previous_state = 0;
    witness.incoming_state = incoming_state;
    witness.replacement_state_after = incoming_state;
    witness.method_rva = OVERLAY_WRITER_METHOD_RVA;
    witness.clear_pre_write_rva = 0x44AF161;
    witness.clear_post_write_rva = 0x44AF168;
    witness.replacement_pre_write_rva = 0x44AF179;
    witness.replacement_post_write_rva = 0x44AF180;
    witness.destination_validated = true;
    witness.same_invocation = true;
    witness.clear_write_confirmed = true;
    witness.replacement_write_confirmed = true;
    witness.re4_image_identity_verified = true;
    return witness;
}

EngineWriterWitnessChain make_chain(
    std::initializer_list<EngineWriterWitness> witnesses,
    uintptr_t observed_state,
    std::atomic<uint64_t>& live_sequence) {
    EngineWriterWitnessChain chain{};
    for (const auto& witness : witnesses) {
        chain.entries[chain.count++] = witness;
    }
    for (size_t index = 0; index < chain.count; ++index) {
        chain.current_store_sequence = std::max(
            chain.current_store_sequence, chain.entries[index].replacement_post_sequence);
    }
    chain.live_store_sequence = &live_sequence;
    chain.stable = true;
    live_sequence.store(chain.current_store_sequence, std::memory_order_release);
    (void)observed_state;
    return chain;
}

EngineWriterChainValidation validate(
    const EngineWriterWitnessChain& chain,
    uintptr_t observed_state,
    uint64_t consumed_sequence = 0) {
    return validate_engine_writer_chain(
        chain, HANDOFF, SAVED, OVERLAY, SLOT, observed_state,
        CONTROL_GENERATION, 4, DEVICE_GENERATION, DEVICE_GENERATION, MODE,
        consumed_sequence);
}

void test_valid_chains() {
    std::atomic<uint64_t> live{};
    auto handoff_to_a = make_transaction(1, 1, HANDOFF, 0xA000);
    auto one = make_chain({ handoff_to_a }, 0xA000, live);
    assert(validate(one, 0xA000));

    auto a_to_b = make_transaction(2, 5, 0xA000, 0xB000);
    auto two = make_chain({ a_to_b, handoff_to_a }, 0xB000, live); // Snapshot may be published out of order.
    assert(validate(two, 0xB000));
    assert(validate(two, 0xB000).chain_length == 2);

    auto b_to_c = make_transaction(3, 9, 0xB000, 0xC000);
    auto three = make_chain({ b_to_c, a_to_b, handoff_to_a }, 0xC000, live);
    assert(validate(three, 0xC000));
    assert(validate(three, 0xC000).chain_length == 3);
}

void test_rejects_gaps_and_incomplete_transactions() {
    std::atomic<uint64_t> live{};
    const auto first = make_transaction(1, 1, HANDOFF, 0xA000);
    auto gap = make_transaction(2, 6, 0xA000, 0xB000);
    auto gapped_chain = make_chain({ first, gap }, 0xB000, live);
    assert(validate(gapped_chain, 0xB000).failure == EngineWriterChainFailure::ChainGap);

    auto missing_event = first;
    missing_event.clear_post_sequence++;
    auto incomplete = make_chain({ missing_event }, 0xA000, live);
    assert(validate(incomplete, 0xA000).failure == EngineWriterChainFailure::InvalidTransaction);

    auto wrong_path = first;
    wrong_path.replacement_pre_write_rva = 0x44AF478;
    auto interleaved = make_chain({ wrong_path }, 0xA000, live);
    assert(validate(interleaved, 0xA000).failure == EngineWriterChainFailure::InvalidTransaction);

    auto failed_write = first;
    failed_write.replacement_write_confirmed = false;
    auto failed = make_chain({ failed_write }, 0xA000, live);
    assert(validate(failed, 0xA000).failure == EngineWriterChainFailure::InvalidTransaction);

    auto non_root = make_transaction(3, 9, SAVED, 0xD000);
    auto no_root = make_chain({ non_root }, 0xD000, live);
    assert(validate(no_root, 0xD000).failure == EngineWriterChainFailure::MissingRoot);
}

void test_rejects_identity_generation_and_terminal_mismatches() {
    std::atomic<uint64_t> live{};
    const auto first = make_transaction(1, 1, HANDOFF, 0xA000);
    auto wrong_slot = first;
    ++wrong_slot.slot;
    auto chain = make_chain({ wrong_slot }, 0xA000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::IdentityMismatch);

    auto wrong_overlay = first;
    ++wrong_overlay.overlay;
    chain = make_chain({ wrong_overlay }, 0xA000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::IdentityMismatch);

    auto wrong_generation = first;
    ++wrong_generation.control_generation;
    chain = make_chain({ wrong_generation }, 0xA000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::GenerationMismatch);

    auto wrong_device = first;
    ++wrong_device.device_reset_generation;
    chain = make_chain({ wrong_device }, 0xA000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::GenerationMismatch);

    auto wrong_mode = first;
    ++wrong_mode.mode_token;
    chain = make_chain({ wrong_mode }, 0xA000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::ModeMismatch);

    auto wrong_thread = make_transaction(2, 5, 0xA000, 0xB000);
    ++wrong_thread.writer_thread_id;
    chain = make_chain({ first, wrong_thread }, 0xB000, live);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::ThreadMismatch);

    chain = make_chain({ first }, 0xB000, live);
    assert(validate(chain, 0xB000).failure == EngineWriterChainFailure::FinalStateMismatch);
    assert(validate(chain, 0xA000, first.clear_pre_sequence).failure == EngineWriterChainFailure::StaleOrReplayed);

    chain.stable = false;
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::UnstableSnapshot);
    chain.stable = true;
    chain.overflow = true;
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::HistoryOverflow);
    chain.overflow = false;
    live.fetch_add(1, std::memory_order_acq_rel);
    assert(validate(chain, 0xA000).failure == EngineWriterChainFailure::StoreSequenceAdvanced);
}

void test_history_capacity_consumption_and_concurrent_snapshots() {
    EngineWriterWitnessHistory history;
    for (size_t index = 0; index < ENGINE_WRITER_CHAIN_CAPACITY; ++index) {
        assert(history.publish(make_transaction(index + 1, index * 4 + 1, HANDOFF, 0xA000 + index)));
    }
    assert(!history.publish(make_transaction(5, 17, HANDOFF, 0xF000)));
    assert(history.snapshot().overflow);

    history.reset();
    const auto first = make_transaction(1, 1, HANDOFF, 0xA000);
    const auto second = make_transaction(2, 5, 0xA000, 0xB000);
    assert(history.publish(first));
    assert(history.publish(second));
    history.consume_through(first.replacement_post_sequence);
    auto after_consume = history.snapshot();
    assert(after_consume.count == 1);
    assert(after_consume.entries[0].invocation_id == second.invocation_id);

    history.reset();
    std::atomic<bool> start{};
    std::thread writer_one([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (uint64_t id = 1; id <= 2; ++id) {
            auto witness = make_transaction(id, id * 4 + 1, HANDOFF, 0x10000 + id);
            witness.incoming_state = static_cast<uintptr_t>(id);
            witness.replacement_state_after = static_cast<uintptr_t>(id);
            (void)history.publish(witness);
        }
    });
    std::thread writer_two([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (uint64_t id = 3; id <= 4; ++id) {
            auto witness = make_transaction(id, id * 4 + 1, HANDOFF, 0x10000 + id);
            witness.incoming_state = static_cast<uintptr_t>(id);
            witness.replacement_state_after = static_cast<uintptr_t>(id);
            (void)history.publish(witness);
        }
    });
    start.store(true, std::memory_order_release);
    do {
        const auto snapshot = history.snapshot();
        assert(snapshot.count <= ENGINE_WRITER_CHAIN_CAPACITY);
        for (size_t index = 0; index < snapshot.count; ++index) {
            const auto& witness = snapshot.entries[index];
            assert(witness.invocation_id != 0);
            assert(witness.incoming_state == witness.replacement_state_after);
            assert(witness.replacement_post_sequence == witness.sequence);
        }
    } while (history.snapshot().count < ENGINE_WRITER_CHAIN_CAPACITY);
    writer_one.join();
    writer_two.join();
    const auto final_snapshot = history.snapshot();
    assert(final_snapshot.count == ENGINE_WRITER_CHAIN_CAPACITY);
    assert(!final_snapshot.overflow);
}

} // namespace

int main() {
    test_valid_chains();
    test_rejects_gaps_and_incomplete_transactions();
    test_rejects_identity_generation_and_terminal_mismatches();
    test_history_capacity_consumption_and_concurrent_snapshots();
    std::cout << "RE4 XeSS writer-chain tests passed\n";
}
