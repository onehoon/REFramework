#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace RE4XeSSSceneView {
class MarkerPendingFrameGate final {
public:
    static constexpr size_t FRAME_CAPACITY = 16;
    static constexpr uint64_t INVALID_FRAME = std::numeric_limits<uint64_t>::max();

    MarkerPendingFrameGate() noexcept {
        for (auto& frame : m_blocked_frames) {
            frame.store(INVALID_FRAME, std::memory_order_relaxed);
        }
    }

    void block(uint64_t frame_id) noexcept {
        if (frame_id == INVALID_FRAME) {
            block_without_frame();
            return;
        }
        m_blocked_frames[frame_id % FRAME_CAPACITY].store(frame_id, std::memory_order_release);
    }

    void block_without_frame() noexcept {
        m_unframed_block.store(true, std::memory_order_release);
    }

    bool is_blocked(uint64_t frame_id) const noexcept {
        if (m_unframed_block.load(std::memory_order_acquire)) {
            return true;
        }
        return frame_id != INVALID_FRAME &&
            m_blocked_frames[frame_id % FRAME_CAPACITY].load(std::memory_order_acquire) == frame_id;
    }

    bool has_unframed_block() const noexcept {
        return m_unframed_block.load(std::memory_order_acquire);
    }

    bool consume(uint64_t frame_id) noexcept {
        bool frame_blocked{};
        if (frame_id != INVALID_FRAME) {
            auto expected = frame_id;
            frame_blocked = m_blocked_frames[frame_id % FRAME_CAPACITY].compare_exchange_strong(
                expected, INVALID_FRAME, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        const bool unframed_blocked = m_unframed_block.exchange(false, std::memory_order_acq_rel);
        return frame_blocked || unframed_blocked;
    }

private:
    std::array<std::atomic<uint64_t>, FRAME_CAPACITY> m_blocked_frames{};
    std::atomic<bool> m_unframed_block{};
};

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
    uint32_t input_height,
    bool output_admission_available = true) noexcept {
    const bool override_applied = result_available && temporal_active && load_observation_valid &&
        output_admission_available && input_width != 0 && input_height != 0;
    return SizeDecision{
        override_applied ? static_cast<float>(input_width) : native_width,
        override_applied ? static_cast<float>(input_height) : native_height,
        override_applied,
    };
}
}
