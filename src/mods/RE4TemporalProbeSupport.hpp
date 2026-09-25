#pragma once

#include <atomic>
#include <cstdint>

namespace re4_temporal_probe {
inline constexpr uint32_t SAMPLE_INTERVAL_CALLBACKS = 60;
inline constexpr uint32_t MAX_SAMPLES = 10;
inline constexpr uint32_t MAX_NON_PRIMARY_SCENE_DIAGNOSTICS = 8;
inline constexpr uint32_t MAX_SCENE_RTVS = 8;
inline constexpr uint32_t MAX_POST_EFFECT_RTVS = 8;

// Defense in depth: the diagnostic and the future bridge are RE4-only.
// Registration is also gated in Mods.cpp, but callbacks must remain inert if
// this object is ever instantiated in another title by mistake.
inline constexpr bool should_process_scene(bool is_re4, bool capture_enabled, const void* scene) noexcept {
    return is_re4 && capture_enabled && scene != nullptr;
}

inline constexpr bool should_process_post_effect(bool is_re4, bool capture_enabled, const void* layer, const void* render_context) noexcept {
    return is_re4 && capture_enabled && layer != nullptr && render_context != nullptr;
}

inline constexpr bool is_primary_scene(bool enabled, bool has_main_camera, bool fully_rendered) noexcept {
    return enabled && has_main_camera && fully_rendered;
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

inline bool should_sample_primary_scene(bool primary_scene, SampleBudget& budget) {
    return primary_scene && budget.should_sample_callback();
}
}
