#include "XeFGCandidateHandoff.hpp"

#include "../../D3D12Hook.hpp"
#include "XeFGCompatibility.hpp"
#include "REFramework.hpp"

#include <spdlog/spdlog.h>

std::mutex XeFGCandidateHandoff::s_pending_mutex{};
std::optional<XeFGBindingCandidate> XeFGCandidateHandoff::s_pending_candidate{};

void XeFGCandidateHandoff::publish(XeFGBindingCandidate candidate) {
    if (g_framework != nullptr) {
        std::unique_lock<std::recursive_mutex> lifecycle_lock{
            g_framework->get_hook_monitor_mutex()};

        // Read the current hook only after taking the lifecycle mutex. This
        // closes the capture-before-hook race during hook replacement.
        if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
            apply_to_live_hook(*hook, candidate);
            return;
        }

        // Store while the lifecycle mutex is still held so hook() cannot miss
        // a candidate between the lookup and release of this lock.
        store_pending(std::move(candidate));
        return;
    }

    // Constructor-time fallback before REFramework publishes its lifecycle mutex.
    store_pending(std::move(candidate));
}

void XeFGCandidateHandoff::store_pending(XeFGBindingCandidate candidate) {
    std::scoped_lock lock{s_pending_mutex};
    s_pending_candidate = std::move(candidate);
}

bool XeFGCandidateHandoff::consume_pending(D3D12Hook& hook) {
    std::optional<XeFGBindingCandidate> pending;
    {
        std::scoped_lock lock{s_pending_mutex};
        pending = std::move(s_pending_candidate);
        s_pending_candidate.reset();
    }

    if (!pending.has_value()) {
        return false;
    }

    // The pending mutex is deliberately released before entering the active
    // binding implementation. A failed bind is not requeued.
    return hook.bind_external_swapchain(
        pending->swapchain.Get(),
        pending->selected_queue.Get(),
        D3D12Hook::SwapchainSource::XeFGInternal,
        pending->observe_only,
        pending->runtime);
}

bool XeFGCandidateHandoff::discard_pending_for_runtime_transition(
    size_t runtime_slot,
    void* context,
    HWND hwnd,
    bool allow_same_hwnd_match,
    const char* reason) {
    std::scoped_lock lock{s_pending_mutex};
    if (!s_pending_candidate.has_value()) {
        return false;
    }

    const auto& pending = *s_pending_candidate;
    const bool exact_runtime_match = context != nullptr
        && pending.runtime.slot == runtime_slot
        && pending.runtime.context == context;
    const bool same_hwnd_match = allow_same_hwnd_match
        && hwnd != nullptr
        && pending.runtime.hwnd == hwnd;
    const char* match = exact_runtime_match ? "exact_runtime" : (same_hwnd_match ? "same_hwnd" : "none");
    if (!exact_runtime_match && !same_hwnd_match) {
        return false;
    }

    if (XeFGCompatibility::is_debug_log_enabled()) {
        spdlog::info("[XeFG][LifecycleDetach] stage = pending_candidate_dropped, reason = {}, match = {}, runtime_slot = {}, context = 0x{:x}, hwnd = 0x{:x}, candidate_context = 0x{:x}, candidate_hwnd = 0x{:x}, swapchain = 0x{:x}",
            reason != nullptr ? reason : "unknown",
            match,
            runtime_slot == XeFGBinding::kInvalidRuntimeSlot ? -1 : static_cast<int64_t>(runtime_slot),
            reinterpret_cast<uintptr_t>(context),
            reinterpret_cast<uintptr_t>(hwnd),
            reinterpret_cast<uintptr_t>(pending.runtime.context),
            reinterpret_cast<uintptr_t>(pending.runtime.hwnd),
            reinterpret_cast<uintptr_t>(pending.swapchain.Get()));
    }
    s_pending_candidate.reset();
    return true;
}

void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {
    if (!hook.apply_xefg_candidate(candidate)) {
        spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
