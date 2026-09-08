#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

class D3D12Hook;

enum class XeFGMonitorAction : uint8_t {
    AllowGenericRecovery,
    PreserveGrace,
    SuppressRuntimeTransition,
    SuppressDetachedUncertain,
    QuarantineSustainedTimeout,
    QuarantineInconsistentState,
};

class XeFGCompatibility {
public:
    static void mark_probe_pending() noexcept;
    static void on_module_loaded(HMODULE module, std::wstring_view base_name, std::wstring_view full_path);
    static void process_pending_work();
    static void install_already_loaded_runtimes();
    static bool is_module_loaded() noexcept;
    static void set_debug_log_enabled(bool enabled) noexcept;
    static bool is_debug_log_enabled() noexcept;
    static XeFGMonitorAction evaluate_hook_monitor_timeout(D3D12Hook& hook) noexcept;
    static bool is_runtime_transition_active() noexcept;
    static void begin_runtime_transition() noexcept;
    static void end_runtime_transition() noexcept;

    class RuntimeTransitionScope {
    public:
        RuntimeTransitionScope() noexcept { begin_runtime_transition(); }
        ~RuntimeTransitionScope() { end_runtime_transition(); }
        RuntimeTransitionScope(const RuntimeTransitionScope&) = delete;
        RuntimeTransitionScope& operator=(const RuntimeTransitionScope&) = delete;
    };
    static int32_t dispatch_init_desc(size_t slot, void* context, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, ID3D12CommandQueue* command_queue, IDXGIFactory2* factory, const void* init_params);
    static int32_t dispatch_get_swapchain(size_t slot, void* context, REFIID riid, void** swap_chain);
    static int32_t dispatch_destroy(size_t slot, void* context);

private:
    static std::atomic<bool> s_module_loaded;
    static std::atomic<bool> s_probe_pending;
    static std::atomic<int64_t> s_first_seen_ms;
    static std::atomic<bool> s_debug_log_enabled;
    static std::atomic<uint32_t> s_runtime_transition_depth;
};
