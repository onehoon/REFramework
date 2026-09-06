#include "XeFGCandidateHandoff.hpp"

#include "../../D3D12Hook.hpp"
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
        pending->observe_only);
}

void XeFGCandidateHandoff::apply_to_live_hook(
    D3D12Hook& hook,
    const XeFGBindingCandidate& candidate) {
    if (!hook.apply_xefg_candidate(candidate)) {
        spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
