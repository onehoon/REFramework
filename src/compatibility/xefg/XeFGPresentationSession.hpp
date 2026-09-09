#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "XeFGBinding.hpp"
#include "XeFGResizeLifecycle.hpp"

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

    XeFGBinding& binding() noexcept { return m_binding; }
    const XeFGBinding& binding() const noexcept { return m_binding; }

    XeFGResizeLifecycle& resize_lifecycle() noexcept { return m_resize_lifecycle; }
    const XeFGResizeLifecycle& resize_lifecycle() const noexcept { return m_resize_lifecycle; }

    XeFGDetachedState& detached_state() noexcept { return m_detached_state; }
    const XeFGDetachedState& detached_state() const noexcept { return m_detached_state; }

    XeFGHookMonitorState& monitor_state() noexcept { return m_monitor_state; }
    const XeFGHookMonitorState& monitor_state() const noexcept { return m_monitor_state; }

    const char* last_monitor_action() const noexcept { return m_last_monitor_action; }
    void set_last_monitor_action(const char* action) noexcept { m_last_monitor_action = action; }

    bool render_boundary_logged() const noexcept { return m_render_boundary_logged; }
    void set_render_boundary_logged(bool value) noexcept { m_render_boundary_logged = value; }

private:
    XeFGBinding m_binding{};
    XeFGResizeLifecycle m_resize_lifecycle{};
    XeFGDetachedState m_detached_state{};
    XeFGHookMonitorState m_monitor_state{};
    const char* m_last_monitor_action{};
    bool m_render_boundary_logged{};
};
