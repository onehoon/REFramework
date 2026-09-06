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
}

std::atomic<bool> XeFGCompatibility::s_module_loaded{false};
std::atomic<bool> XeFGCompatibility::s_probe_pending{false};
std::atomic<int64_t> XeFGCompatibility::s_first_seen_ms{-1};

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

    spdlog::info("[XeFG][Module] name = {}, base = 0x{:x}, full_path = {}, first_seen_ms = {}",
        utility::narrow(std::wstring{base_name}), reinterpret_cast<uintptr_t>(module),
        utility::narrow(std::wstring{full_path}), s_first_seen_ms.load(std::memory_order_relaxed));

    const auto init_from_swap_chain = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain");
    const auto init_from_swap_chain_desc = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get_swap_chain_ptr = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");
    spdlog::info("[XeFG][Exports] InitFromSwapChain = 0x{:x} / {}, InitFromSwapChainDesc = 0x{:x} / {}, GetSwapChainPtr = 0x{:x} / {}",
        reinterpret_cast<uintptr_t>(init_from_swap_chain), init_from_swap_chain != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(init_from_swap_chain_desc), init_from_swap_chain_desc != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(get_swap_chain_ptr), get_swap_chain_ptr != nullptr ? "present" : "missing");

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

    spdlog::info(
        "[XeFG][HookMonitor] action = preserve_binding, "
        "reason = present_timeout, generation = {}, "
        "swapchain = 0x{:x}, queue = 0x{:x}",
        hook.get_xefg_binding_generation(),
        reinterpret_cast<uintptr_t>(hook.get_swap_chain()),
        reinterpret_cast<uintptr_t>(hook.get_command_queue()));
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

    auto observation_scope = XeFGDiscovery::observe_init(
        original, context, hwnd, swap_chain_desc, fullscreen_desc, command_queue, factory, init_params);
    const auto& observation = observation_scope.observation;
    spdlog::info("[XeFG][InitDesc] context = 0x{:x}, hwnd = 0x{:x}, queue = 0x{:x}, factory = 0x{:x}, width = {}, height = {}, format = {}, buffer_count = {}, flags = 0x{:x}, result = {}",
        reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(hwnd), reinterpret_cast<uintptr_t>(command_queue), reinterpret_cast<uintptr_t>(factory),
        swap_chain_desc != nullptr ? swap_chain_desc->Width : 0, swap_chain_desc != nullptr ? swap_chain_desc->Height : 0,
        swap_chain_desc != nullptr ? swap_chain_desc->Format : DXGI_FORMAT_UNKNOWN, swap_chain_desc != nullptr ? swap_chain_desc->BufferCount : 0,
        swap_chain_desc != nullptr ? swap_chain_desc->Flags : 0, observation.init_result);
    spdlog::info("[XeFG][InitDesc] slot = {}, module = 0x{:x}, context = 0x{:x}, result = {}",
        slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context), observation.init_result);

    XeFGBindingCandidateResult decision{};
    {
        std::scoped_lock lock{g_xefg_state_mutex};
        decision = XeFGDiscovery::build_binding_candidate(observation);
        if (!decision.accepted()) {
            spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = {}",
                reinterpret_cast<uintptr_t>(observation.internal_swapchain), decision.reject_reason);
        } else {
            const auto& candidate = *decision.candidate;
            spdlog::info("[XeFG][Bind] candidate = 0x{:x}, accepted = true, reason = {}",
                reinterpret_cast<uintptr_t>(candidate.swapchain.Get()), decision.bind_reason);
            spdlog::info("[XeFG][P2.1Probe] mode = {}, selected_queue = 0x{:x}, render_callbacks = {}, reason = {}",
                candidate.observe_only ? (candidate.relation == XeFGQueueRelation::SameComIdentity ? "observe_only_same_queue" : "observe_only_invalid_presentation_queue") : "presentation_queue_render",
                reinterpret_cast<uintptr_t>(candidate.selected_queue.Get()), !candidate.observe_only, decision.probe_reason);
        }
    }
    if (decision.accepted()) {
        XeFGCandidateHandoff::publish(std::move(*decision.candidate));
    }
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
    spdlog::info("[XeFG][RuntimeDispatch] api = GetSwapChainPtr, slot = {}, module = 0x{:x}, context = 0x{:x}, result = {}",
        slot, reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(context), result);
    if (result == kXefgSuccess && swap_chain != nullptr && *swap_chain != nullptr) {
        std::scoped_lock lock{g_xefg_state_mutex};
        const auto internal_candidate = XeFGDiscovery::current_internal_swapchain_for_diagnostics();
        spdlog::info("[XeFG][PublicProxy] context = 0x{:x}, swapchain = 0x{:x}, internal_same = {}",
            reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(*swap_chain), *swap_chain == internal_candidate);
    }
    return result;
}
