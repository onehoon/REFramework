#include "XeFGCompatibility.hpp"

#include <chrono>
#include <mutex>
#include <tlhelp32.h>

#include <spdlog/spdlog.h>

#include "XeFGRuntimeRegistry.hpp"
#include "XeFGCandidateHandoff.hpp"
#include "XeFGDiscovery.hpp"
#include "XeFGResult.hpp"
#include "D3D12Hook.hpp"
#include "REFramework.hpp"
#include "utility/String.hpp"

namespace {
const auto g_diagnostic_start_time = std::chrono::steady_clock::now();
std::mutex g_xefg_state_mutex{};

const char* queue_relation_name(XeFGQueueRelation relation) noexcept {
    switch (relation) {
    case XeFGQueueRelation::SameComIdentity: return "same_com_identity";
    case XeFGQueueRelation::DistinctSameDevice: return "distinct_same_device";
    case XeFGQueueRelation::DeviceMismatch: return "device_mismatch";
    case XeFGQueueRelation::InitQueueUnavailable: return "init_queue_unavailable";
    case XeFGQueueRelation::PresentationQueueUnavailable: return "presentation_queue_unavailable";
    case XeFGQueueRelation::PresentationQueueNotDirect: return "presentation_queue_not_direct";
    default: return "unknown";
    }
}

const char* monitor_action_name(XeFGMonitorAction action) noexcept {
    switch (action) {
    case XeFGMonitorAction::AllowGenericRecovery: return "allow_generic_recovery";
    case XeFGMonitorAction::PreserveGrace: return "preserve_grace";
    case XeFGMonitorAction::SuppressRuntimeTransition: return "suppress_rehook";
    case XeFGMonitorAction::SuppressDetachedUncertain: return "suppress_rehook";
    case XeFGMonitorAction::QuarantineSustainedTimeout: return "quarantine";
    case XeFGMonitorAction::QuarantineInconsistentState: return "quarantine";
    default: return "unknown";
    }
}

int64_t runtime_slot_for_log(size_t slot) noexcept {
    return slot == XeFGBinding::kInvalidRuntimeSlot ? -1 : static_cast<int64_t>(slot);
}

struct ActiveBindingSnapshot {
    XeFGBinding::RuntimeLifecycleSnapshot binding{};
    uint64_t last_resize_event_id{};
    const char* last_resize_kind{"none"};
    int64_t last_present_age_ms{-1};
};

ActiveBindingSnapshot active_binding_snapshot() noexcept {
    if (g_framework == nullptr) {
        return {};
    }

    std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};
    const auto* hook = D3D12Hook::current_xefg_handoff_target();
    if (hook == nullptr) {
        return {};
    }

    return {
        hook->get_xefg_lifecycle_snapshot(),
        hook->get_xefg_last_resize_event_id(),
        hook->get_xefg_last_resize_kind(),
        hook->get_last_present_age_ms(),
    };
}

void prepare_for_xefg_runtime_transition(
    size_t runtime_slot,
    void* context,
    HWND hwnd,
    bool allow_same_hwnd_match,
    const char* reason) {
    if (g_framework == nullptr) {
        XeFGCandidateHandoff::discard_pending_for_runtime_transition(
            runtime_slot, context, hwnd, allow_same_hwnd_match, reason);
        return;
    }

    // Lock order is lifecycle mutex -> pending-candidate mutex. The lock is
    // released when this helper returns, before Intel Init/Destroy executes.
    std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};
    XeFGCandidateHandoff::discard_pending_for_runtime_transition(
        runtime_slot, context, hwnd, allow_same_hwnd_match, reason);
    if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
        hook->detach_xefg_binding_for_runtime_transition(
            runtime_slot, context, hwnd, allow_same_hwnd_match, reason);
    }
}

void log_runtime_lifecycle(const char* stage, size_t slot, HMODULE module, void* context, HWND hwnd, int32_t result = 0, bool has_result = false, const ActiveBindingSnapshot* cached_snapshot = nullptr) {
    if (!XeFGCompatibility::is_debug_log_enabled()) {
        return;
    }

    const auto snapshot = cached_snapshot != nullptr ? *cached_snapshot : active_binding_snapshot();
    const auto& binding = snapshot.binding;
    const auto context_match = binding.active && context != nullptr && binding.runtime.context == context;
    const auto hwnd_match = hwnd != nullptr && binding.runtime.hwnd == hwnd;
    const auto slot_match = binding.active && binding.runtime.slot == slot;
    spdlog::info("[XeFG][RuntimeLifecycle] stage = {}, slot = {}, module = 0x{:x}, context = 0x{:x}, hwnd = 0x{:x}, result = {}, active_binding_present = {}, active_generation = {}, active_context = 0x{:x}, active_hwnd = 0x{:x}, active_runtime_slot = {}, active_swapchain = 0x{:x}, active_queue = 0x{:x}, active_device = 0x{:x}, active_observe_only = {}, context_match = {}, hwnd_match = {}, runtime_slot_match = {}, last_resize_event_id = {}, last_resize_kind = {}, last_present_age_ms = {}",
        stage,
        slot,
        reinterpret_cast<uintptr_t>(module),
        reinterpret_cast<uintptr_t>(context),
        reinterpret_cast<uintptr_t>(hwnd),
        has_result ? result : 0,
        binding.active,
        binding.generation,
        reinterpret_cast<uintptr_t>(binding.runtime.context),
        reinterpret_cast<uintptr_t>(binding.runtime.hwnd),
        runtime_slot_for_log(binding.runtime.slot),
        reinterpret_cast<uintptr_t>(binding.swapchain),
        reinterpret_cast<uintptr_t>(binding.queue),
        reinterpret_cast<uintptr_t>(binding.device),
        binding.observe_only,
        context_match,
        hwnd_match,
        slot_match,
        snapshot.last_resize_event_id,
        snapshot.last_resize_kind,
        snapshot.last_present_age_ms);
}
}

std::atomic<bool> XeFGCompatibility::s_module_loaded{false};
std::atomic<bool> XeFGCompatibility::s_probe_pending{false};
std::atomic<int64_t> XeFGCompatibility::s_first_seen_ms{-1};
std::atomic<bool> XeFGCompatibility::s_debug_log_enabled{false};
std::atomic<uint32_t> XeFGCompatibility::s_runtime_transition_depth{0};

void XeFGCompatibility::set_debug_log_enabled(bool enabled) noexcept {
    s_debug_log_enabled.store(enabled, std::memory_order_relaxed);
}

bool XeFGCompatibility::is_debug_log_enabled() noexcept {
    return s_debug_log_enabled.load(std::memory_order_relaxed);
}

void XeFGCompatibility::mark_probe_pending() noexcept {
    s_module_loaded.store(true, std::memory_order_release);
    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);
    s_probe_pending.store(true, std::memory_order_release);
}

void XeFGCompatibility::on_module_loaded(HMODULE module, std::wstring_view base_name, std::wstring_view full_path) {
    if (module == nullptr) {
        return;
    }

    s_module_loaded.store(true, std::memory_order_relaxed);
    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);

    if (is_debug_log_enabled()) {
        spdlog::info("[XeFG][Module] name = {}, base = 0x{:x}, full_path = {}, first_seen_ms = {}",
            utility::narrow(std::wstring{base_name}), reinterpret_cast<uintptr_t>(module),
            utility::narrow(std::wstring{full_path}), s_first_seen_ms.load(std::memory_order_relaxed));
    }

    const auto init_from_swap_chain = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain");
    const auto init_from_swap_chain_desc = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get_swap_chain_ptr = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");
    const auto destroy = GetProcAddress(module, "xefgSwapChainDestroy");
    if (is_debug_log_enabled()) {
        spdlog::info("[XeFG][Exports] InitFromSwapChain = 0x{:x} / {}, InitFromSwapChainDesc = 0x{:x} / {}, GetSwapChainPtr = 0x{:x} / {}, Destroy = 0x{:x} / {}",
            reinterpret_cast<uintptr_t>(init_from_swap_chain), init_from_swap_chain != nullptr ? "present" : "missing",
            reinterpret_cast<uintptr_t>(init_from_swap_chain_desc), init_from_swap_chain_desc != nullptr ? "present" : "missing",
            reinterpret_cast<uintptr_t>(get_swap_chain_ptr), get_swap_chain_ptr != nullptr ? "present" : "missing",
            reinterpret_cast<uintptr_t>(destroy), destroy != nullptr ? "present" : "missing");
    }

    XeFGRuntimeRegistry::instance().install_for_module(module, full_path);
}

void XeFGCompatibility::install_already_loaded_runtimes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::warn("[XeFG][RuntimeRegistry] module enumeration failed, error = {}", GetLastError());
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"libxess_fg.dll") == 0) {
                on_module_loaded(entry.hModule, std::wstring_view{entry.szModule}, std::wstring_view{entry.szExePath});
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void XeFGCompatibility::process_pending_work() {
    if (!s_probe_pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    install_already_loaded_runtimes();
}

bool XeFGCompatibility::is_module_loaded() noexcept {
    return s_module_loaded.load(std::memory_order_relaxed);
}

bool XeFGCompatibility::is_runtime_transition_active() noexcept {
    return s_runtime_transition_depth.load(std::memory_order_acquire) != 0;
}

void XeFGCompatibility::begin_runtime_transition() noexcept {
    s_runtime_transition_depth.fetch_add(1, std::memory_order_acq_rel);
}

void XeFGCompatibility::end_runtime_transition() noexcept {
    auto depth = s_runtime_transition_depth.load(std::memory_order_acquire);
    while (depth != 0
        && !s_runtime_transition_depth.compare_exchange_weak(
            depth, depth - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

XeFGMonitorAction XeFGCompatibility::evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept {
    XeFGMonitorAction action = XeFGMonitorAction::AllowGenericRecovery;
    const char* reason = "xefg_state_safe";

    if (is_runtime_transition_active()) {
        action = XeFGMonitorAction::SuppressRuntimeTransition;
        reason = "runtime_transition";
    } else if (!hook.has_xefg_monitor_state()) {
        action = XeFGMonitorAction::AllowGenericRecovery;
    } else if (hook.has_xefg_detached_state()) {
        action = XeFGMonitorAction::SuppressDetachedUncertain;
        reason = "detached_uncertain";
    } else if (!hook.has_consistent_active_xefg_binding()) {
        action = XeFGMonitorAction::QuarantineInconsistentState;
        reason = "binding_identity_inconsistent";
    } else if (hook.note_xefg_monitor_timeout() == XeFGHookMonitorState::TimeoutClass::Sustained) {
        action = XeFGMonitorAction::QuarantineSustainedTimeout;
        reason = "sustained_present_timeout";
    } else {
        action = XeFGMonitorAction::PreserveGrace;
        reason = "present_timeout";
    }

    if (hook.note_xefg_monitor_action(reason) && is_debug_log_enabled()) {
        const auto binding = hook.get_xefg_lifecycle_snapshot();
        spdlog::info("[XeFG][HookMonitor] action = {}, reason = {}, generation = {}, runtime_slot = {}, context = 0x{:x}, swapchain = 0x{:x}, hook_target = 0x{:x}, present_entry_count = {}, present_age_ms = {}, timeout_count = {}, transition_depth = {}, detached_uncertain = {}",
            monitor_action_name(action),
            reason,
            binding.generation,
            binding.runtime.slot == XeFGBinding::kInvalidRuntimeSlot ? -1 : static_cast<int64_t>(binding.runtime.slot),
            reinterpret_cast<uintptr_t>(binding.runtime.context),
            reinterpret_cast<uintptr_t>(binding.swapchain),
            reinterpret_cast<uintptr_t>(hook.get_xefg_monitor_binding_key().hook_target),
            hook.get_present_entry_count(),
            hook.get_last_present_age_ms(),
            hook.get_xefg_timeout_count(),
            s_runtime_transition_depth.load(std::memory_order_acquire),
            hook.has_xefg_detached_state());
        hook.log_hook_monitor_snapshot("xefg_monitor_decision");
    }

    return action;
}

int32_t XeFGCompatibility::dispatch_init_desc(size_t slot, void* context, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue, IDXGIFactory2* factory, const void* init_params) {
    XeFGRuntimeRegistry::InitFn original{};
    HMODULE module{};
    {
        const auto target = XeFGRuntimeRegistry::instance().resolve_init(slot);
        if (!target) {
            spdlog::error("[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = {}, action = fail, reason = runtime_not_active", slot);
            return -1;
        }
        original = target->original;
        module = target->module;
    }

    RuntimeTransitionScope transition_scope;

    if (is_debug_log_enabled()) {
        spdlog::info("[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = {}, module = 0x{:x}, context = 0x{:x}",
            slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context));
    }

    log_runtime_lifecycle("pre_init", slot, module, context, hwnd);
    prepare_for_xefg_runtime_transition(slot, context, hwnd, true, "reinit");

    auto observation_scope = XeFGDiscovery::observe_init(
        original, slot, context, hwnd, swap_chain_desc, fullscreen_desc, command_queue, factory, init_params);
    const auto& observation = observation_scope.observation;
    if (is_debug_log_enabled()) {
    spdlog::info("[XeFG][InitDesc] context = 0x{:x}, hwnd = 0x{:x}, queue = 0x{:x}, factory = 0x{:x}, width = {}, height = {}, format = {}, buffer_count = {}, flags = 0x{:x}, result = {}",
        reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(hwnd), reinterpret_cast<uintptr_t>(command_queue), reinterpret_cast<uintptr_t>(factory),
        swap_chain_desc != nullptr ? swap_chain_desc->Width : 0, swap_chain_desc != nullptr ? swap_chain_desc->Height : 0,
        swap_chain_desc != nullptr ? swap_chain_desc->Format : DXGI_FORMAT_UNKNOWN, swap_chain_desc != nullptr ? swap_chain_desc->BufferCount : 0,
        swap_chain_desc != nullptr ? swap_chain_desc->Flags : 0, observation.init_result);
    spdlog::info("[XeFG][InitDesc] slot = {}, module = 0x{:x}, context = 0x{:x}, result = {}",
        slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context), observation.init_result);
    }

    XeFGBindingCandidateResult decision{};
    {
        std::scoped_lock lock{g_xefg_state_mutex};
        decision = XeFGDiscovery::build_binding_candidate(observation);
        if (!decision.accepted()) {
            spdlog::warn("[XeFG][Bind] accepted = false, reason = {}", decision.reject_reason);
        } else {
            const auto& candidate = *decision.candidate;
            spdlog::info("[XeFG][Bind] accepted = true, reason = {}, mode = {}, queue_relation = {}",
                decision.bind_reason,
                candidate.observe_only ? "observe_only" : "render",
                queue_relation_name(candidate.relation));
            if (is_debug_log_enabled()) spdlog::info("[XeFG][P2.1Probe] mode = {}, selected_queue = 0x{:x}, render_callbacks = {}, reason = {}",
                candidate.observe_only ? (candidate.relation == XeFGQueueRelation::SameComIdentity ? "observe_only_same_queue" : "observe_only_invalid_presentation_queue") : "presentation_queue_render",
                reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()), !candidate.observe_only, decision.probe_reason);
        }
    }
    if (decision.accepted()) {
        XeFGCandidateHandoff::publish(std::move(*decision.candidate));
    }
    log_runtime_lifecycle("init_return", slot, module, context, hwnd, observation.init_result, true);
    return observation.init_result;
}

int32_t XeFGCompatibility::dispatch_get_swapchain(size_t slot, void* context, REFIID riid, void** swap_chain) {
    XeFGRuntimeRegistry::GetSwapchainFn original{};
    HMODULE module{};
    {
        const auto target = XeFGRuntimeRegistry::instance().resolve_get_swapchain(slot);
        if (!target) {
            spdlog::error("[XeFG][RuntimeDispatch] api = GetSwapChainPtr, slot = {}, action = fail, reason = runtime_not_active", slot);
            return -1;
        }
        original = target->original;
        module = target->module;
    }

    const auto result = original(context, riid, swap_chain);
    if (is_debug_log_enabled()) {
        spdlog::info("[XeFG][RuntimeDispatch] api = GetSwapChainPtr, slot = {}, module = 0x{:x}, context = 0x{:x}, result = {}",
            slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context), result);
    }
    if (xefg_result::succeeded(result) && swap_chain != nullptr && *swap_chain != nullptr) {
        std::scoped_lock lock{g_xefg_state_mutex};
        const auto internal_candidate = XeFGDiscovery::current_internal_swapchain_for_diagnostics();
        if (is_debug_log_enabled()) spdlog::info("[XeFG][PublicProxy] context = 0x{:x}, swapchain = 0x{:x}, internal_same = {}",
            reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(*swap_chain), *swap_chain == internal_candidate);
    }
    return result;
}

int32_t XeFGCompatibility::dispatch_destroy(size_t slot, void* context) {
    XeFGRuntimeRegistry::DestroyFn original{};
    HMODULE module{};
    {
        const auto target = XeFGRuntimeRegistry::instance().resolve_destroy(slot);
        if (!target) {
            spdlog::error("[XeFG][RuntimeDispatch] api = Destroy, slot = {}, action = fail, reason = runtime_not_active", slot);
            return -1;
        }
        original = target->original;
        module = target->module;
    }

    RuntimeTransitionScope transition_scope;

    const auto binding_before_destroy = active_binding_snapshot();
    log_runtime_lifecycle("destroy_enter", slot, module, context, nullptr, 0, false, &binding_before_destroy);
    prepare_for_xefg_runtime_transition(slot, context, nullptr, false, "destroy");
    const auto result = original(context);
    if (g_framework != nullptr) {
        // The vendor call has returned, so reacquire the lifecycle mutex before
        // mutating hook-monitor state. RuntimeTransitionScope remains active
        // until this function returns, so monitor recovery stays suppressed.
        std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};
        if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
            hook->note_xefg_destroy_result(slot, context, result);
        }
    } else if (auto* hook = D3D12Hook::current_xefg_handoff_target(); hook != nullptr) {
        hook->note_xefg_destroy_result(slot, context, result);
    }
    log_runtime_lifecycle("destroy_return", slot, module, context, nullptr, result, true, &binding_before_destroy);
    return result;
}
