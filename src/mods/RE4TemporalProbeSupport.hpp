#pragma once

#include <atomic>
#include <cstdint>

namespace re4_temporal_probe {
inline constexpr uint32_t MAX_TEMPORAL_SAMPLES = 32;

// Defense in depth: the diagnostic and future bridge are RE4-only.
inline constexpr bool should_process_scene_update(
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
    uint32_t reserve_frame(uint32_t frame) {
        if (frame == 0) {
            return 0;
        }

        auto last = m_last_frame.load(std::memory_order_relaxed);
        while (last != frame) {
            if (m_last_frame.compare_exchange_weak(last, frame, std::memory_order_relaxed)) {
                auto current = m_samples.load(std::memory_order_relaxed);

                while (current < MAX_TEMPORAL_SAMPLES) {
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
