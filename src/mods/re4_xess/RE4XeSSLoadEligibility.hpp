#pragma once

#include <cstdint>

// RE4-only, side-effect-free load-window admission predicate.
// This is not an OutputHandoff/Present eligibility or a full frame/view admission token.
namespace RE4XeSSLoadEligibility {
struct Snapshot {
    bool observation_valid{};
    bool pause_observed{};
    bool pause_active{};
    bool normal_inhibit_baseline_valid{};
    bool inhibit_departure_pending{};
    bool load_transition_active{};
    bool startup_rebaseline_pending{};
};

struct UpdateWindow {
    uint64_t sequence_before{};
    uint64_t sequence_after{};
    uint64_t overlap_count{};
    uint32_t update_thread_id{};
    uint32_t active_before{};
    uint32_t active_after{};
    Snapshot state{};

    [[nodiscard]] constexpr bool overlapped() const noexcept {
        return active_before != 0 || active_after != 0 || sequence_before != sequence_after;
    }
};

[[nodiscard]] constexpr uint32_t encode(const Snapshot& state) noexcept {
    return (static_cast<uint32_t>(state.observation_valid) << 0) |
        (static_cast<uint32_t>(state.pause_observed) << 1) |
        (static_cast<uint32_t>(state.pause_active) << 2) |
        (static_cast<uint32_t>(state.normal_inhibit_baseline_valid) << 3) |
        (static_cast<uint32_t>(state.inhibit_departure_pending) << 4) |
        (static_cast<uint32_t>(state.load_transition_active) << 5) |
        (static_cast<uint32_t>(state.startup_rebaseline_pending) << 6);
}

[[nodiscard]] constexpr Snapshot decode(uint32_t bits) noexcept {
    return Snapshot{
        (bits & (1u << 0)) != 0,
        (bits & (1u << 1)) != 0,
        (bits & (1u << 2)) != 0,
        (bits & (1u << 3)) != 0,
        (bits & (1u << 4)) != 0,
        (bits & (1u << 5)) != 0,
        (bits & (1u << 6)) != 0,
    };
}

[[nodiscard]] constexpr bool allows_temporal_rendering(const Snapshot& state) noexcept {
    return state.observation_valid && state.pause_observed && !state.pause_active &&
        state.normal_inhibit_baseline_valid && !state.inhibit_departure_pending &&
        !state.load_transition_active && !state.startup_rebaseline_pending;
}
} // namespace RE4XeSSLoadEligibility
