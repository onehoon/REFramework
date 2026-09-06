#include "XeFGRuntimeRegistry.hpp"

#include <spdlog/spdlog.h>

#include "XeFGCompatibility.hpp"
#include "utility/String.hpp"

namespace {

template <size_t Slot>
int32_t WINAPI xefg_init_desc_thunk(
    void* context, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    ID3D12CommandQueue* command_queue, IDXGIFactory2* factory,
    const void* init_params) {
    return XeFGCompatibility::dispatch_init_desc(
        Slot, context, hwnd, swap_chain_desc, fullscreen_desc, command_queue,
        factory, init_params);
}

template <size_t Slot>
int32_t WINAPI xefg_get_swapchain_thunk(void* context, REFIID riid, void** swap_chain) {
    return XeFGCompatibility::dispatch_get_swapchain(Slot, context, riid, swap_chain);
}

constexpr std::array<XeFGRuntimeRegistry::InitFn, 8> kInitThunks{
    &xefg_init_desc_thunk<0>, &xefg_init_desc_thunk<1>,
    &xefg_init_desc_thunk<2>, &xefg_init_desc_thunk<3>,
    &xefg_init_desc_thunk<4>, &xefg_init_desc_thunk<5>,
    &xefg_init_desc_thunk<6>, &xefg_init_desc_thunk<7>,
};

constexpr std::array<XeFGRuntimeRegistry::GetSwapchainFn, 8> kGetSwapchainThunks{
    &xefg_get_swapchain_thunk<0>, &xefg_get_swapchain_thunk<1>,
    &xefg_get_swapchain_thunk<2>, &xefg_get_swapchain_thunk<3>,
    &xefg_get_swapchain_thunk<4>, &xefg_get_swapchain_thunk<5>,
    &xefg_get_swapchain_thunk<6>, &xefg_get_swapchain_thunk<7>,
};

} // namespace

XeFGRuntimeRegistry& XeFGRuntimeRegistry::instance() {
    static XeFGRuntimeRegistry registry;
    return registry;
}

XeFGRuntimeRegistry::RuntimeHook* XeFGRuntimeRegistry::find_by_module_locked(HMODULE module) {
    for (auto& entry : m_runtimes) {
        if (entry && entry->module == module) {
            return &*entry;
        }
    }
    return nullptr;
}

XeFGRuntimeRegistry::RuntimeHook* XeFGRuntimeRegistry::find_by_slot_locked(size_t slot) {
    if (slot >= m_runtimes.size() || !m_runtimes[slot]) {
        return nullptr;
    }
    return &*m_runtimes[slot];
}

std::optional<size_t> XeFGRuntimeRegistry::allocate_slot_locked() const {
    for (size_t slot = 0; slot < m_runtimes.size(); ++slot) {
        if (!m_runtimes[slot]) {
            return slot;
        }
    }
    return std::nullopt;
}

bool XeFGRuntimeRegistry::install_for_module(HMODULE module, std::wstring_view full_path) {
    if (module == nullptr) {
        return false;
    }

    std::scoped_lock lock{m_mutex};
    if (find_by_module_locked(module) != nullptr) {
        spdlog::info("[XeFG][RuntimeRegistry] action = duplicate, module = 0x{:x}, path = {}",
            reinterpret_cast<uintptr_t>(module), utility::narrow(std::wstring{full_path}));
        return true;
    }

    const auto init_export = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    if (init_export == nullptr) {
        spdlog::warn("[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, path = {}, reason = init_desc_missing",
            reinterpret_cast<uintptr_t>(module), utility::narrow(std::wstring{full_path}));
        return false;
    }

    const auto slot = allocate_slot_locked();
    if (!slot) {
        spdlog::error("[XeFG][RuntimeRegistry] action = rejected, module = 0x{:x}, reason = capacity_exceeded, capacity = {}",
            reinterpret_cast<uintptr_t>(module), m_runtimes.size());
        return false;
    }

    auto& runtime = m_runtimes[*slot].emplace();
    runtime.module = module;
    runtime.path = std::wstring{full_path};
    runtime.slot = *slot;
    runtime.init_desc_export = init_export;
    runtime.get_swapchain_export = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");
    runtime.state = InstallState::Installing;
    runtime.init_desc_hook = std::make_unique<FunctionHook>(
        Address{reinterpret_cast<void*>(init_export)},
        Address{reinterpret_cast<void*>(kInitThunks[*slot])});
    if (!runtime.init_desc_hook->create()) {
        spdlog::error("[XeFG][RuntimeRegistry] action = rejected, slot = {}, module = 0x{:x}, reason = init_hook_failed",
            *slot, reinterpret_cast<uintptr_t>(module));
        m_runtimes[*slot].reset();
        return false;
    }

    if (runtime.get_swapchain_export != nullptr) {
        runtime.get_swapchain_hook = std::make_unique<FunctionHook>(
            Address{reinterpret_cast<void*>(runtime.get_swapchain_export)},
            Address{reinterpret_cast<void*>(kGetSwapchainThunks[*slot])});
        if (!runtime.get_swapchain_hook->create()) {
            spdlog::warn("[XeFG][RuntimeRegistry] api = GetSwapChainPtr, action = optional_hook_failed, slot = {}, module = 0x{:x}",
                *slot, reinterpret_cast<uintptr_t>(module));
            runtime.get_swapchain_hook.reset();
        }
    }

    runtime.state = InstallState::Active;
    spdlog::info("[XeFG][RuntimeRegistry] action = installed, slot = {}, module = 0x{:x}, get_swapchain = {}",
        *slot, reinterpret_cast<uintptr_t>(module), runtime.get_swapchain_export != nullptr ? "present" : "missing");
    if (XeFGCompatibility::is_debug_log_enabled()) {
        spdlog::info("[XeFG][RuntimeRegistry] slot = {}, path = {}, init_desc = 0x{:x}, get_swapchain = 0x{:x}",
            *slot, utility::narrow(runtime.path), reinterpret_cast<uintptr_t>(runtime.init_desc_export),
            reinterpret_cast<uintptr_t>(runtime.get_swapchain_export));
    }
    return true;
}

std::optional<XeFGRuntimeRegistry::InitDispatchTarget>
XeFGRuntimeRegistry::resolve_init(size_t slot) {
    std::scoped_lock lock{m_mutex};
    auto* runtime = find_by_slot_locked(slot);
    if (runtime == nullptr || runtime->state != InstallState::Active || runtime->init_desc_hook == nullptr) {
        return std::nullopt;
    }
    return InitDispatchTarget{
        runtime->module,
        reinterpret_cast<InitFn>(runtime->init_desc_hook->get_original()),
    };
}

std::optional<XeFGRuntimeRegistry::GetSwapchainDispatchTarget>
XeFGRuntimeRegistry::resolve_get_swapchain(size_t slot) {
    std::scoped_lock lock{m_mutex};
    auto* runtime = find_by_slot_locked(slot);
    if (runtime == nullptr || runtime->state != InstallState::Active || runtime->get_swapchain_hook == nullptr) {
        return std::nullopt;
    }
    return GetSwapchainDispatchTarget{
        runtime->module,
        reinterpret_cast<GetSwapchainFn>(runtime->get_swapchain_hook->get_original()),
    };
}
