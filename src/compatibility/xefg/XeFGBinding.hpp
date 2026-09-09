#pragma once

#include <cstdint>
#include <cstddef>
#include <utility>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>

class XeFGBinding {
public:
    static constexpr size_t kInvalidRuntimeSlot = static_cast<size_t>(-1);

    struct RuntimeIdentity {
        size_t slot{kInvalidRuntimeSlot};
        void* context{};
        HWND hwnd{};
    };

    struct RuntimeLifecycleSnapshot {
        bool active{};
        uint64_t generation{};
        RuntimeIdentity runtime{};
        IDXGISwapChain3* swapchain{};
        ID3D12CommandQueue* queue{};
        ID3D12Device4* device{};
        bool observe_only{};
    };
    struct IdentityChange {
        bool swapchain_changed{};
        bool queue_changed{};
        bool mode_changed{};

        bool changed() const noexcept {
            return swapchain_changed || queue_changed || mode_changed;
        }

        const char* reason() const noexcept;
    };

    bool complete() const noexcept;
    bool active() const noexcept;
    IDXGISwapChain3* swapchain() const noexcept;
    ID3D12CommandQueue* queue() const noexcept;
    ID3D12Device4* device() const noexcept;
    bool observe_only() const noexcept;
    uint64_t generation() const noexcept;
    bool matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool observe_only) const noexcept;
    IdentityChange compare(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool observe_only) const noexcept;
    bool aliases_match(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, ID3D12Device4* device) const noexcept;
    RuntimeLifecycleSnapshot lifecycle_snapshot() const noexcept;
    bool runtime_identity_matches(size_t slot, void* context) const noexcept;

    void commit_initial(IDXGISwapChain3* swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only, RuntimeIdentity runtime = {});
    void commit_same_swapchain_update(Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only, RuntimeIdentity runtime = {});
    void commit_replacement(IDXGISwapChain3* swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only, RuntimeIdentity runtime = {});
    void refresh_runtime_identity(RuntimeIdentity runtime) noexcept;
    void clear() noexcept;

private:
    IDXGISwapChain3* m_swapchain{}; // borrowed; hook lifetime is bounded by the caller
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
    RuntimeIdentity m_runtime{};
};
