#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "XeFGBinding.hpp"

struct XeFGMonitorBindingKey {
    uint64_t generation{};
    size_t runtime_slot{XeFGBinding::kInvalidRuntimeSlot};
    void* runtime_context{};
    IDXGISwapChain3* swapchain{};
    void* hook_target{};

    bool operator==(const XeFGMonitorBindingKey& other) const noexcept {
        return generation == other.generation
            && runtime_slot == other.runtime_slot
            && runtime_context == other.runtime_context
            && swapchain == other.swapchain
            && hook_target == other.hook_target;
    }
};

class XeFGHookMonitorState {
public:
    enum class TimeoutClass : uint8_t {
        Grace,
        Sustained,
    };

    TimeoutClass note_timeout(
        const XeFGMonitorBindingKey& key,
        uint64_t present_entry_count,
        int64_t present_age_ms) noexcept;

    void clear() noexcept;

    uint32_t consecutive_timeouts() const noexcept {
        return m_consecutive_timeouts;
    }

private:
    static constexpr uint32_t kSustainedTimeoutThreshold = 3;
    static constexpr int64_t kMinimumSustainedPresentAgeMs = 20000;

    XeFGMonitorBindingKey m_key{};
    uint64_t m_last_present_entry_count{};
    uint32_t m_consecutive_timeouts{};
    bool m_initialized{};
};

struct XeFGDetachedState {
    bool active{};
    XeFGBinding::RuntimeIdentity previous_runtime{};
    uint64_t previous_generation{};
    const char* reason{};
};

class XeFGPresentationSession {
public:
    XeFGPresentationSession() = default;
};
