#include "XeFGCompatibility.hpp"

#include <chrono>
#include <mutex>
#include <tlhelp32.h>

#include <spdlog/spdlog.h>

#include "XeFGRuntimeRegistry.hpp"
#include "XeFGCandidateHandoff.hpp"
#include "XeFGDiscovery.hpp"
#include "D3D12Hook.hpp"
#include "utility/String.hpp"

namespace {
const auto g_diagnostic_start_time = std::chrono::steady_clock::now();
constexpr int32_t kXefgSuccess = 0;
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

int64_t runtime_slot_for_log(size_t slot) noexcept {
    return slot == XeFGBinding::kInvalidRuntimeSlot ? -1 : static_cast<int64_t>(slot);
}

XeFGBinding::RuntimeLifecycleSnapshot active_binding_snapshot() noexcept {
    const auto* hook = D3D12Hook::current_xefg_handoff_target();
    return hook != nullptr ? hook->get_xefg_lifecycle_snapshot() : XeFGBinding::RuntimeLifecycleSnapshot{};
}

void log_runtime_lifecycle(const char* stage, size_t slot, HMODULE module, void* context, HWND hwnd, int32_t result = 0, bool has_result = false, const XeFGBinding::RuntimeLifecycleSnapshot* cached_binding = nullptr) {
    if (!XeFGCompatibility::is_debug_log_enabled()) {
        return;
    }

    const auto binding = cached_binding != nullptr ? *cached_binding : active_binding_snapshot();
    const auto* hook = cached_binding == nullptr ? D3D12Hook::current_xefg_handoff_target() : nullptr;
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
        hook != nullptr ? hook->get_xefg_last_resize_event_id() : 0,
        hook != nullptr ? hook->get_xefg_last_resize_kind() : "none",
        hook != nullptr ? hook->get_last_present_age_ms() : -1);
}
}

std::atomic<bool> XeFGCompatibility::s_module_loaded{false};
std::atomic<bool> XeFGCompatibility::s_probe_pending{false};
std::atomic<int64_t> XeFGCompatibility::s_first_seen_ms{-1};
std::atomic<bool> XeFGCompatibility::s_debug_log_enabled{false};

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

bool XeFGCompatibility::should_preserve_active_binding_on_monitor_timeout(D3D12Hook& hook) noexcept {
    if (!hook.has_active_xefg_instance_binding()) {
        return false;
    }

    spdlog::info("[XeFG][HookMonitor] action = preserve_binding, reason = present_timeout, generation = {}",
        hook.get_xefg_binding_generation());
    hook.log_hook_monitor_snapshot("xefg_rehook_suppressed");
    return true;
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

    if (is_debug_log_enabled()) {
        spdlog::info("[XeFG][RuntimeDispatch] api = InitFromSwapChainDesc, slot = {}, module = 0x{:x}, context = 0x{:x}",
            slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context));
    }

    log_runtime_lifecycle("pre_init", slot, module, context, hwnd);

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
    if (result == kXefgSuccess && swap_chain != nullptr && *swap_chain != nullptr) {
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

    const auto binding_before_destroy = active_binding_snapshot();
    log_runtime_lifecycle("destroy_enter", slot, module, context, nullptr);
    const auto result = original(context);
    log_runtime_lifecycle("destroy_return", slot, module, context, nullptr, result, true, &binding_before_destroy);
    return result;
}
