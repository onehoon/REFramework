#pragma once

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

[[nodiscard]] constexpr bool allows_temporal_rendering(const Snapshot& state) noexcept {
    return state.observation_valid && state.pause_observed && !state.pause_active &&
        state.normal_inhibit_baseline_valid && !state.inhibit_departure_pending &&
        !state.load_transition_active && !state.startup_rebaseline_pending;
}
} // namespace RE4XeSSLoadEligibility
