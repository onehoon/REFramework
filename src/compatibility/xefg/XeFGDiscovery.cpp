#include "XeFGDiscovery.hpp"

#include <wrl/client.h>

#include <spdlog/spdlog.h>

std::recursive_mutex XeFGDiscovery::s_transaction_mutex{};
XeFGDiscovery::ActiveTransaction XeFGDiscovery::s_active{};
std::unique_ptr<VtableHook> XeFGDiscovery::s_factory_hook{};
std::atomic<IDXGISwapChain1*> XeFGDiscovery::s_diagnostic_candidate{nullptr};

XeFGDiscovery::ObservationScope XeFGDiscovery::observe_init(
    InitFn original,
    void* context,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue,
    IDXGIFactory2* factory,
    const void* init_params) {
    std::unique_lock transaction_lock{s_transaction_mutex};

    s_active.observation = {};
    s_active.observation.context = context;
    s_active.observation.hwnd = hwnd;
    s_active.observation.init_queue = command_queue;
    s_active.observation.factory = factory;
    s_diagnostic_candidate.store(nullptr, std::memory_order_release);
    s_factory_hook.reset();

    if (factory != nullptr) {
        try {
            s_factory_hook = std::make_unique<VtableHook>(Address{factory});
            if (!s_factory_hook->hook_method(15, Address{reinterpret_cast<void*>(&create_swapchain_for_hwnd)})) {
                s_factory_hook.reset();
            }
        } catch (...) {
            s_factory_hook.reset();
        }
    }

    const auto result = original(
        context, hwnd, swap_chain_desc, fullscreen_desc, command_queue, factory, init_params);
    s_active.observation.init_result = result;
    s_factory_hook.reset();

    return ObservationScope{std::move(s_active.observation), std::move(transaction_lock)};
}

IDXGISwapChain1* XeFGDiscovery::current_internal_swapchain_for_diagnostics() noexcept {
    return s_diagnostic_candidate.load(std::memory_order_acquire);
}

HRESULT WINAPI XeFGDiscovery::create_swapchain_for_hwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output,
    IDXGISwapChain1** swap_chain) {
    if (s_factory_hook == nullptr) {
        return E_FAIL;
    }

    using CreateSwapchainFn = HRESULT (WINAPI*)(
        IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    const auto original = s_factory_hook->get_method<CreateSwapchainFn>(15);
    const auto result = original(
        factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);

    if (SUCCEEDED(result) && swap_chain != nullptr && *swap_chain != nullptr) {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentation_queue;
        if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&presentation_queue)))) {
            s_active.observation.presentation_queue = presentation_queue.Get();
        }

        s_active.observation.internal_swapchain = *swap_chain;
        s_active.observation.factory_create_succeeded = true;
        s_diagnostic_candidate.store(*swap_chain, std::memory_order_release);
        spdlog::info(
            "[XeFG][InternalSwapchain] context = 0x{:x}, candidate = 0x{:x}, presentation_queue = 0x{:x}, provisional = true",
            reinterpret_cast<uintptr_t>(s_active.observation.context),
            reinterpret_cast<uintptr_t>(*swap_chain),
            reinterpret_cast<uintptr_t>(s_active.observation.presentation_queue));
    }

    return result;
}
