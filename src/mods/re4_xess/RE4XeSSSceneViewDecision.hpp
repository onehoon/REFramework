#pragma once

#include <cstdint>

namespace RE4XeSSSceneView {
struct SizeDecision {
    float effective_width{};
    float effective_height{};
    bool override_applied{};
};

inline SizeDecision decide_size(
    bool result_available,
    float native_width,
    float native_height,
    bool temporal_active,
    bool load_observation_valid,
    uint32_t input_width,
    uint32_t input_height) noexcept {
    const bool override_applied = result_available && temporal_active && load_observation_valid &&
        input_width != 0 && input_height != 0;
    return SizeDecision{
        override_applied ? static_cast<float>(input_width) : native_width,
        override_applied ? static_cast<float>(input_height) : native_height,
        override_applied,
    };
}
}
