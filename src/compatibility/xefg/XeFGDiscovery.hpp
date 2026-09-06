#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/VtableHook.hpp"

class XeFGDiscovery {
public:
    using InitFn = int32_t (WINAPI*)(
        void*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
        ID3D12CommandQueue*, IDXGIFactory2*, const void*);

    struct Observation {
        void* context{};
        HWND hwnd{};
        ID3D12CommandQueue* init_queue{};
        ID3D12CommandQueue* presentation_queue{};
        IDXGIFactory2* factory{};
        IDXGISwapChain1* internal_swapchain{};
        bool factory_create_succeeded{};
        int32_t init_result{-1};
    };

    class ObservationScope {
    public:
        Observation observation{};

        ObservationScope(Observation&& value, std::unique_lock<std::recursive_mutex>&& lock)
            : observation{std::move(value)}, transaction_lock{std::move(lock)} {}
        ObservationScope(ObservationScope&&) = default;
        ObservationScope& operator=(ObservationScope&&) = default;
        ObservationScope(const ObservationScope&) = delete;
        ObservationScope& operator=(const ObservationScope&) = delete;

    private:
        std::unique_lock<std::recursive_mutex> transaction_lock;
    };

    static ObservationScope observe_init(
        InitFn original,
        void* context,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        ID3D12CommandQueue* command_queue,
        IDXGIFactory2* factory,
        const void* init_params);

    static IDXGISwapChain1* current_internal_swapchain_for_diagnostics() noexcept;

private:
    struct ActiveTransaction {
        Observation observation{};
    };

    static HRESULT WINAPI create_swapchain_for_hwnd(
        IDXGIFactory2* factory,
        IUnknown* device,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
        IDXGIOutput* restrict_to_output,
        IDXGISwapChain1** swap_chain);

    static std::recursive_mutex s_transaction_mutex;
    static ActiveTransaction s_active;
    static std::unique_ptr<VtableHook> s_factory_hook;
    static std::atomic<IDXGISwapChain1*> s_diagnostic_candidate;
};
