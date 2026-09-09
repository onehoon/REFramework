#pragma once

#include <iostream>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi")

#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>

#include "utility/PointerHook.hpp"
#include "utility/FunctionHook.hpp"
#include "utility/VtableHook.hpp"
#include "compatibility/xefg/XeFGBinding.hpp"
#include "compatibility/xefg/XeFGPresentationSession.hpp"
#include "compatibility/xefg/XeFGResizeLifecycle.hpp"

class XeFGCandidateHandoff;
class XeFGCompatibility;
struct XeFGBindingCandidate;

class D3D12Hook
{
public:
	friend class XeFGCompatibility;
	enum class SwapchainSource : uint8_t {
		Native,
		XeFGInternal,
	};

	typedef std::function<void(D3D12Hook&)> OnPresentFn;
	typedef std::function<void(D3D12Hook&)> OnResizeBuffersFn;
    typedef std::function<void(D3D12Hook&)> OnResizeTargetFn;
    typedef std::function<void(D3D12Hook&)> OnCreateSwapChainFn;

	D3D12Hook() = default;
	virtual ~D3D12Hook();

	bool hook();
	bool unhook();

	bool bind_external_swapchain(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue, SwapchainSource source, bool xefg_p21_observe_only = false, XeFGBinding::RuntimeIdentity runtime = {});
    bool apply_xefg_candidate(const XeFGBindingCandidate& candidate);

    bool is_hooked() {
        return m_hooked;
    }

    void on_present(OnPresentFn fn) {
        m_on_present = fn;
    }

    void on_post_present(OnPresentFn fn) {
        m_on_post_present = fn;
    }

    void on_resize_buffers(OnResizeBuffersFn fn) {
        m_on_resize_buffers = fn;
    }

    void on_resize_target(OnResizeTargetFn fn) {
        m_on_resize_target = fn;
    }

    /*void on_create_swap_chain(OnCreateSwapChainFn fn) {
        m_on_create_swap_chain = fn;
    }*/

    ID3D12Device4* get_device() const {
        return m_device;
    }

    IDXGISwapChain3* get_swap_chain() const {
        return m_swap_chain;
    }

    auto get_swapchain_0() { return m_swapchain_0; }
    auto get_swapchain_1() { return m_swapchain_1; }

    ID3D12CommandQueue* get_command_queue() const {
        return m_command_queue;
    }

    UINT get_display_width() const {
        return m_display_width;
    }

    UINT get_display_height() const {
        return m_display_height;
    }

    UINT get_render_width() const {
        return m_render_width;
    }

    UINT get_render_height() const {
        return m_render_height;
    }

    bool is_inside_present() const {
        return m_inside_present;
    }

    uint64_t get_present_entry_count() const {
        return m_present_entry_count.load(std::memory_order_relaxed);
    }

    int64_t get_last_present_age_ms() const noexcept;

    uint64_t get_xefg_last_resize_event_id() const { return m_xefg_session.last_resize_event_id(); }
    uint64_t get_xefg_binding_generation() const { return m_xefg_session.resize_diagnostic_snapshot().binding_generation; }
    const char* get_xefg_last_resize_kind() const;

    static uint32_t get_command_queue_offset_for_diagnostics() {
        return s_command_queue_offset;
    }

    void log_hook_monitor_snapshot(std::string_view event) const;

    bool is_proton_swapchain() const {
        return m_using_proton_swapchain;
    }
    
    bool is_framegen_swapchain() const {
        return m_using_frame_generation_swapchain;
    }

	SwapchainSource get_swapchain_source() const {
		return m_swapchain_source;
	}

    bool is_xefg_observe_only() const {
        return m_xefg_session.binding().observe_only();
    }

    XeFGBinding::RuntimeLifecycleSnapshot get_xefg_lifecycle_snapshot() const noexcept {
        return m_xefg_session.binding().lifecycle_snapshot();
    }

    static D3D12Hook* current_xefg_handoff_target() noexcept;

    // Detach only the active XeFG instance relationship before its runtime
    // destroys or reinitializes the borrowed proxy.
    bool detach_xefg_binding_for_runtime_transition(
        size_t runtime_slot,
        void* context,
        HWND hwnd,
        bool allow_same_hwnd_match,
        const char* reason);

    void note_xefg_destroy_result(size_t runtime_slot, void* context, int32_t result) noexcept;

    void ignore_next_present() {
        m_ignore_next_present = true;
    }

    static void hook_streamline(HMODULE dlssg_module = nullptr);

    using XefgResizeEventKind = XeFGResizeLifecycle::EventKind;

protected:
    bool has_active_xefg_instance_binding() const noexcept;
    bool has_consistent_active_xefg_binding() const noexcept;
    bool has_xefg_monitor_state() const noexcept;
    bool has_xefg_detached_state() const noexcept { return m_xefg_session.detached_uncertain(); }
    XeFGMonitorBindingKey get_xefg_monitor_binding_key() const noexcept;
    XeFGHookMonitorState::TimeoutClass note_xefg_monitor_timeout() noexcept;
    uint32_t get_xefg_timeout_count() const noexcept { return m_xefg_session.monitor_timeout_count(); }
    bool note_xefg_monitor_action(const char* action) noexcept;
    void clear_xefg_monitor_state() noexcept;
    XeFGPresentationSession::PhysicalBindingView get_xefg_physical_binding_view() const noexcept;
    XeFGPresentationSession::MonitorEvaluation evaluate_xefg_monitor_timeout(bool runtime_transition_active) noexcept;
    bool is_xefg_source() const noexcept { return m_swapchain_source == SwapchainSource::XeFGInternal; }
    bool is_tracked_xefg_instance(IDXGISwapChain3* swapchain) const noexcept;
    uint64_t begin_tracked_xefg_resize_event(IDXGISwapChain3* swapchain, XeFGResizeLifecycle::EventKind kind, bool top_level);
    void hook_impl();
	static HRESULT WINAPI present1(IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
    static HRESULT present_common(IDXGISwapChain3* swap_chain, const char* kind, void* original_present, std::function<HRESULT()> original_call, bool allow_phase_transition);
    struct XeFGHookPreparation {
        std::unique_ptr<VtableHook> hook{};
        const char* failure_reason{};

        bool ready() const noexcept {
            return hook != nullptr;
        }
    };

    XeFGHookPreparation prepare_xefg_instance_hook(IDXGISwapChain3* swapchain);
    bool apply_xefg_binding_request(
        IDXGISwapChain3* swapchain,
        ID3D12CommandQueue* command_queue,
        bool observe_only,
        XeFGBinding::RuntimeIdentity runtime);
    void arm_xefg_resize_transition_hold(uint64_t event_id, bool renderer_reset_performed);
    void complete_xefg_resize_transition_hold(uint64_t completion_event_id, XefgResizeEventKind completion_kind, HRESULT result);
    void clear_xefg_resize_transition_hold(const char* reason);
    void log_xefg_resize_event(uint64_t event_id, XefgResizeEventKind kind, const char* stage, IDXGISwapChain3* swap_chain, void* original_fn, HRESULT result = S_OK, bool has_result = false) const;
    void log_xefg_post_resize_present(const XeFGPresentationSession::PostResizePresentDecision& decision,
        IDXGISwapChain3* swap_chain, const char* kind, void* original_fn) const;
    bool external_binding_matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue, SwapchainSource source, bool xefg_observe_only) const;
    void sync_xefg_binding_aliases() noexcept;

    ID3D12Device4* m_device{ nullptr };
    IDXGISwapChain3* m_swap_chain{ nullptr };
    IDXGISwapChain3* m_swapchain_0{};
    IDXGISwapChain3* m_swapchain_1{};
    ID3D12CommandQueue* m_command_queue{ nullptr };
    XeFGPresentationSession m_xefg_session{};
    UINT m_display_width{ NULL };
    UINT m_display_height{ NULL };
    UINT m_render_width{ NULL };
    UINT m_render_height{ NULL };

    static inline uint32_t s_command_queue_offset{};
    static inline uint32_t s_proton_swapchain_offset{};

    bool m_using_proton_swapchain{ false };
    bool m_using_frame_generation_swapchain{ false };
	SwapchainSource m_swapchain_source{ SwapchainSource::Native };
    bool m_hooked{ false };
    bool m_is_phase_1{ true };
    bool m_inside_present{false};
    bool m_ignore_next_present{false};
    std::atomic<uint64_t> m_present_entry_count{0};
    std::atomic<std::chrono::steady_clock::rep> m_last_present_entry_ticks{0};
    void* m_last_logged_present_swapchain{ nullptr };
    void* m_last_logged_present_target{ nullptr };
    bool m_last_logged_present_phase_1{ true };
    bool m_last_logged_present_xefg{ false };

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<VtableHook> m_swapchain_hook{};

    struct Streamline {
        static void* link_swapchain_to_cmd_queue(void* rcx, void* rdx, void* r8, void* r9);

        std::unique_ptr<FunctionHook> link_swapchain_to_cmd_queue_hook{};
        std::mutex hook_mutex{};
        bool setup{ false };
    };

    static inline Streamline s_streamline{};

    // This is static because unhooking it seems to cause a crash sometimes
    static inline std::unique_ptr<PointerHook> s_create_swapchain_hook{};
    static inline void** s_factory_vtable{ nullptr };
    static inline void** s_swapchain_vtable{ nullptr };
    OnPresentFn m_on_present{ nullptr };
    OnPresentFn m_on_post_present{ nullptr };
    OnResizeBuffersFn m_on_resize_buffers{ nullptr };
    OnResizeTargetFn m_on_resize_target{ nullptr };
    //OnCreateSwapChainFn m_on_create_swap_chain{ nullptr };
    
    static HRESULT WINAPI present(IDXGISwapChain3* swap_chain, uint64_t sync_interval, uint64_t flags, void* r9);
    static HRESULT WINAPI resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI resize_buffers1(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags, const UINT* creation_node_mask, IUnknown* const* present_queues);
    static HRESULT WINAPI resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters);
    static HRESULT WINAPI create_swapchain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain1** swap_chain);
};

