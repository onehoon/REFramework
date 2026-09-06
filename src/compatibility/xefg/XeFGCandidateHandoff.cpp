#include "XeFGCandidateHandoff.hpp"

#include "../../D3D12Hook.hpp"
#include "REFramework.hpp"

#include <spdlog/spdlog.h>

std::mutex XeFGCandidateHandoff::s_pending_mutex{};
std::optional<XeFGBindingCandidate> XeFGCandidateHandoff::s_pending_candidate{};

namespace {

const char* binding_change_reason(bool swapchain_changed, bool queue_changed, bool mode_changed) {
    const auto change_count = static_cast<int>(swapchain_changed)
        + static_cast<int>(queue_changed)
        + static_cast<int>(mode_changed);

    if (change_count > 1) {
        return "multiple_fields_changed";
    }
    if (swapchain_changed) {
        return "swapchain_changed";
    }
    if (queue_changed) {
        return "queue_changed";
    }
    if (mode_changed) {
        return "mode_changed";
    }
    return "identical";
}

} // namespace

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
    const auto has_active_xefg = hook.is_hooked()
        && hook.get_swap_chain() != nullptr
        && hook.get_swapchain_source() == D3D12Hook::SwapchainSource::XeFGInternal;

    if (has_active_xefg) {
        const auto swapchain_changed = hook.get_swap_chain() != candidate.swapchain.Get();
        const auto queue_changed = hook.get_command_queue() != candidate.selected_queue.Get();
        const auto mode_changed = hook.is_xefg_observe_only() != candidate.observe_only;
        const auto changed = swapchain_changed || queue_changed || mode_changed;
        const auto reason = binding_change_reason(swapchain_changed, queue_changed, mode_changed);

        spdlog::info("[XeFG][BindingGate] action = {}, reason = {}, old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, old_queue = 0x{:x}, new_queue = 0x{:x}, old_observe_only = {}, new_observe_only = {}",
            changed ? "rebind" : "unchanged",
            reason,
            reinterpret_cast<uintptr_t>(hook.get_swap_chain()),
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()),
            reinterpret_cast<uintptr_t>(hook.get_command_queue()),
            reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()),
            hook.is_xefg_observe_only(),
            candidate.observe_only);

        if (changed && !hook.replace_xefg_binding(
                candidate.swapchain.Get(), candidate.selected_queue.Get(), candidate.observe_only, reason)) {
            spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = rebind_failed",
                reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
        }
        return;
    }

    const auto replacing_active_non_xefg = hook.is_hooked()
        && hook.get_swap_chain() != nullptr
        && hook.get_swapchain_source() != D3D12Hook::SwapchainSource::XeFGInternal;
    if (replacing_active_non_xefg) {
        spdlog::info("[XeFG][Bind] resetting active D3D12 renderer before XeFG bind");
        g_framework->on_reset();
    }

    if (!hook.bind_external_swapchain(
            candidate.swapchain.Get(), candidate.selected_queue.Get(),
            D3D12Hook::SwapchainSource::XeFGInternal, candidate.observe_only)) {
        spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
            reinterpret_cast<uintptr_t>(candidate.swapchain.Get()));
    }
}
