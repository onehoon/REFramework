#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace re4_temporal_probe {
inline constexpr uint32_t MAX_TEMPORAL_SAMPLES = 32;
inline constexpr uint32_t RESET_HISTORY_SCENARIO = 8;
inline constexpr uint32_t LOAD_STATE_SCENARIO = 9;
inline constexpr uint32_t RESET_WATCH_MAX_SAMPLES = 4096;
inline constexpr uint32_t LOAD_STATE_MAX_SAMPLES = 8192;
inline constexpr uint32_t JITTER_PHASE_COUNT = 4;
inline constexpr uint32_t MV_READBACK_FIRST_SAMPLE = 5;
inline constexpr uint32_t MV_READBACK_SAMPLE_COUNT = 16;
inline constexpr uint32_t MV_READBACK_LAST_SAMPLE =
    MV_READBACK_FIRST_SAMPLE + MV_READBACK_SAMPLE_COUNT - 1;
inline constexpr uint32_t MV_SAMPLE_GRID_SIZE = 3;
inline constexpr uint32_t MV_SAMPLE_POINT_COUNT = MV_SAMPLE_GRID_SIZE * MV_SAMPLE_GRID_SIZE;
inline constexpr uint64_t MV_READBACK_POINT_STRIDE = 512;
inline constexpr uint64_t MV_READBACK_BUFFER_SIZE =
    MV_SAMPLE_POINT_COUNT * MV_READBACK_POINT_STRIDE;

inline constexpr bool is_directional_mv_scenario(int scenario) noexcept {
    return scenario >= 1 && scenario <= 4;
}

inline constexpr bool is_reset_history_scenario(int scenario) noexcept {
    return scenario == static_cast<int>(RESET_HISTORY_SCENARIO);
}

inline constexpr bool is_load_state_scenario(int scenario) noexcept {
    return scenario == static_cast<int>(LOAD_STATE_SCENARIO);
}

inline constexpr bool is_horizontal_mv_scenario(int scenario) noexcept {
    return scenario == 1 || scenario == 2;
}

inline constexpr bool is_vertical_mv_scenario(int scenario) noexcept {
    return scenario == 3 || scenario == 4;
}

inline constexpr bool should_readback_mv_sample(int scenario, uint32_t sample) noexcept {
    return is_directional_mv_scenario(scenario) &&
        sample >= MV_READBACK_FIRST_SAMPLE &&
        sample <= MV_READBACK_LAST_SAMPLE;
}

struct MotionPixelCandidate {
    float x{};
    float y{};
};

struct PerspectiveDepthTerms {
    float p22{};
    float p32{};
};

inline constexpr bool valid_clip_planes(float near_plane, float far_plane) noexcept {
    return near_plane > 0.0f && far_plane > near_plane;
}

inline constexpr PerspectiveDepthTerms normal_depth_terms(
    float near_plane,
    float far_plane) noexcept {
    if (!valid_clip_planes(near_plane, far_plane)) {
        return {};
    }

    const auto denominator = near_plane - far_plane;
    return {
        far_plane / denominator,
        (far_plane * near_plane) / denominator,
    };
}

inline constexpr PerspectiveDepthTerms inverted_depth_terms(
    float near_plane,
    float far_plane) noexcept {
    if (!valid_clip_planes(near_plane, far_plane)) {
        return {};
    }

    const auto denominator = far_plane - near_plane;
    return {
        near_plane / denominator,
        (far_plane * near_plane) / denominator,
    };
}

inline constexpr float absf(float value) noexcept {
    return value < 0.0f ? -value : value;
}

inline constexpr float depth_terms_error(
    PerspectiveDepthTerms actual,
    PerspectiveDepthTerms expected) noexcept {
    return absf(actual.p22 - expected.p22) + absf(actual.p32 - expected.p32);
}

struct ScreenPoint {
    float x{};
    float y{};
};

inline constexpr ScreenPoint pixel_to_ndc(
    ScreenPoint pixel,
    uint32_t width,
    uint32_t height) noexcept {
    if (width == 0 || height == 0) {
        return {};
    }

    return {
        (2.0f * pixel.x / static_cast<float>(width)) - 1.0f,
        1.0f - (2.0f * pixel.y / static_cast<float>(height)),
    };
}

inline constexpr ScreenPoint ndc_to_pixel(
    ScreenPoint ndc,
    uint32_t width,
    uint32_t height) noexcept {
    if (width == 0 || height == 0) {
        return {};
    }

    return {
        (ndc.x + 1.0f) * 0.5f * static_cast<float>(width),
        (1.0f - ndc.y) * 0.5f * static_cast<float>(height),
    };
}

inline constexpr MotionPixelCandidate historical_motion_pixel_candidate(
    float r_snorm,
    float g_snorm,
    uint32_t render_width,
    uint32_t render_height) noexcept {
    return {
        r_snorm * static_cast<float>(render_width) / 2.0f,
        g_snorm * -static_cast<float>(render_height) / 2.0f,
    };
}

struct MVSamplePoint {
    uint32_t x{};
    uint32_t y{};
};

inline constexpr MVSamplePoint mv_sample_point(
    uint32_t index,
    uint32_t width,
    uint32_t height) noexcept {
    if (index >= MV_SAMPLE_POINT_COUNT || width == 0 || height == 0) {
        return {};
    }

    const auto column = index % MV_SAMPLE_GRID_SIZE;
    const auto row = index / MV_SAMPLE_GRID_SIZE;

    const auto x = ((column + 1) * width) / (MV_SAMPLE_GRID_SIZE + 1);
    const auto y = ((row + 1) * height) / (MV_SAMPLE_GRID_SIZE + 1);

    return {
        x < width ? x : width - 1,
        y < height ? y : height - 1,
    };
}

inline constexpr float decode_snorm16(int16_t value) noexcept {
    return value == INT16_MIN
        ? -1.0f
        : static_cast<float>(value) / 32767.0f;
}

struct JitterOffset {
    float x{};
    float y{};
};

inline constexpr std::array<JitterOffset, JITTER_PHASE_COUNT> TEST_JITTER_PIXELS{{
    {+0.5f, +0.5f},
    {-0.5f, +0.5f},
    {-0.5f, -0.5f},
    {+0.5f, -0.5f},
}};

inline constexpr JitterOffset jitter_pixels_for_sample(uint32_t sample) noexcept {
    if (sample == 0) {
        return {};
    }

    return TEST_JITTER_PIXELS[(sample - 1) % JITTER_PHASE_COUNT];
}

inline constexpr JitterOffset projection_jitter_from_pixels(
    JitterOffset pixel_jitter,
    uint32_t render_width,
    uint32_t render_height) noexcept {
    if (render_width == 0 || render_height == 0) {
        return {};
    }

    // Historical REFramework temporal-upscaler convention.
    return {
        2.0f * pixel_jitter.x / static_cast<float>(render_width),
        -2.0f * pixel_jitter.y / static_cast<float>(render_height),
    };
}

// Defense in depth: the diagnostic and future bridge are RE4-only.
inline constexpr bool should_process_scene(
    bool is_re4,
    bool capture_enabled,
    const void* scene) noexcept {
    return is_re4 && capture_enabled && scene != nullptr;
}

inline constexpr bool should_process_camera_projection(
    bool is_re4,
    bool capture_enabled,
    const void* camera,
    const void* result) noexcept {
    return is_re4 && capture_enabled && camera != nullptr && result != nullptr;
}

class FrameBudget {
public:
    uint32_t reserve_frame(uint32_t frame, uint32_t max_samples = MAX_TEMPORAL_SAMPLES) {
        if (frame == 0 || max_samples == 0) {
            return 0;
        }

        auto last = m_last_frame.load(std::memory_order_relaxed);
        while (last != frame) {
            if (m_last_frame.compare_exchange_weak(last, frame, std::memory_order_relaxed)) {
                auto current = m_samples.load(std::memory_order_relaxed);

                while (current < max_samples) {
                    if (m_samples.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                        return current + 1;
                    }
                }

                return 0;
            }
        }

        return 0;
    }

    void reset() {
        m_last_frame.store(0, std::memory_order_relaxed);
        m_samples.store(0, std::memory_order_relaxed);
    }

    uint32_t sample_count() const {
        return m_samples.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> m_last_frame{0};
    std::atomic<uint32_t> m_samples{0};
};
}
