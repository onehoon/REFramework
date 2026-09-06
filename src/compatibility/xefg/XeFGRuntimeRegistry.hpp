#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/FunctionHook.hpp"

class XeFGRuntimeRegistry {
public:
    using InitFn = int32_t (WINAPI*)(
        void*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*,
        IDXGIFactory2*, const void*);
    using GetSwapchainFn = int32_t (WINAPI*)(void*, REFIID, void**);

    struct InitDispatchTarget {
        HMODULE module{};
        InitFn original{};
    };

    struct GetSwapchainDispatchTarget {
        HMODULE module{};
        GetSwapchainFn original{};
    };

    static XeFGRuntimeRegistry& instance();

    bool install_for_module(HMODULE module, std::wstring_view full_path);
    std::optional<InitDispatchTarget> resolve_init(size_t slot);
    std::optional<GetSwapchainDispatchTarget> resolve_get_swapchain(size_t slot);

private:
    static constexpr size_t kMaxRuntimes = 8;

    enum class InstallState : uint8_t {
        Empty,
        Installing,
        Active,
    };

    struct RuntimeHook {
        HMODULE module{};
        std::wstring path{};
        size_t slot{};
        FARPROC init_desc_export{};
        FARPROC get_swapchain_export{};
        std::unique_ptr<FunctionHook> init_desc_hook{};
        std::unique_ptr<FunctionHook> get_swapchain_hook{};
        InstallState state{InstallState::Empty};
    };

    RuntimeHook* find_by_module_locked(HMODULE module);
    RuntimeHook* find_by_slot_locked(size_t slot);
    std::optional<size_t> allocate_slot_locked() const;

    std::mutex m_mutex{};
    std::array<std::optional<RuntimeHook>, kMaxRuntimes> m_runtimes{};
};
