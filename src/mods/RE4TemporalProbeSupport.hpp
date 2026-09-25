#pragma once

#include <atomic>
#include <cstdint>

namespace re4_temporal_probe {
inline constexpr uint32_t SAMPLE_INTERVAL_CALLBACKS = 60;
inline constexpr uint32_t MAX_SAMPLES = 240;

inline constexpr bool should_process_scene(bool capture_enabled, const void* scene) noexcept {
    return capture_enabled && scene != nullptr;
}

inline constexpr bool is_primary_scene(bool enabled, bool has_main_camera, bool fully_rendered) noexcept {
    return enabled && has_main_camera && fully_rendered;
}

inline constexpr bool has_scene_local_color_candidate(bool primary_scene, const void* prepare_output) noexcept {
    return primary_scene && prepare_output != nullptr;
}

class SampleBudget {
public:
    bool should_sample_callback() {
        const auto callback = m_scene_draw_callbacks.fetch_add(1, std::memory_order_relaxed);
        return callback % SAMPLE_INTERVAL_CALLBACKS == 0;
    }

    uint32_t reserve_sample() {
        auto current = m_samples.load(std::memory_order_relaxed);

        while (current < MAX_SAMPLES) {
            if (m_samples.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
                return current + 1;
            }
        }

        return 0;
    }

    void reset() {
        m_scene_draw_callbacks.store(0, std::memory_order_relaxed);
        m_samples.store(0, std::memory_order_relaxed);
    }

    uint32_t callback_count() const {
        return m_scene_draw_callbacks.load(std::memory_order_relaxed);
    }

    uint32_t sample_count() const {
        return m_samples.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> m_scene_draw_callbacks{0};
    std::atomic<uint32_t> m_samples{0};
};
}
