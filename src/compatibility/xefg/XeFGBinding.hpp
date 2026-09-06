#pragma once

#include <cstdint>
#include <utility>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>

class XeFGBinding {
public:
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

    void commit_initial(Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only);
    void commit_same_swapchain_update(Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only);
    void commit_replacement(Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only);
    void clear() noexcept;

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device4> m_device{};
    uint64_t m_generation{};
    bool m_observe_only{};
};
