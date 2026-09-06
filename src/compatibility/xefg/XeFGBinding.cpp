#include "XeFGBinding.hpp"

const char* XeFGBinding::IdentityChange::reason() const noexcept {
    const auto count = static_cast<int>(swapchain_changed)
        + static_cast<int>(queue_changed)
        + static_cast<int>(mode_changed);
    if (count > 1) return "multiple_fields_changed";
    if (swapchain_changed) return "swapchain_changed";
    if (queue_changed) return "queue_changed";
    if (mode_changed) return "mode_changed";
    return "identical";
}

bool XeFGBinding::complete() const noexcept {
    return m_swapchain != nullptr && m_queue != nullptr && m_device != nullptr;
}

bool XeFGBinding::active() const noexcept {
    return complete() && m_generation != 0;
}

IDXGISwapChain3* XeFGBinding::swapchain() const noexcept { return m_swapchain.Get(); }
ID3D12CommandQueue* XeFGBinding::queue() const noexcept { return m_queue.Get(); }
ID3D12Device4* XeFGBinding::device() const noexcept { return m_device.Get(); }
bool XeFGBinding::observe_only() const noexcept { return m_observe_only; }
uint64_t XeFGBinding::generation() const noexcept { return m_generation; }

bool XeFGBinding::matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool observe_only) const noexcept {
    return active() && m_swapchain.Get() == swapchain && m_queue.Get() == queue && m_observe_only == observe_only;
}

XeFGBinding::IdentityChange XeFGBinding::compare(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool observe_only) const noexcept {
    return {m_swapchain.Get() != swapchain, m_queue.Get() != queue, m_observe_only != observe_only};
}

bool XeFGBinding::aliases_match(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, ID3D12Device4* device) const noexcept {
    return complete() && m_swapchain.Get() == swapchain && m_queue.Get() == queue && m_device.Get() == device;
}

void XeFGBinding::commit_initial(Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only) {
    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = 1;
}

void XeFGBinding::commit_same_swapchain_update(Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only) {
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    ++m_generation;
}

void XeFGBinding::commit_replacement(Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain, Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue, Microsoft::WRL::ComPtr<ID3D12Device4> device, bool observe_only) {
    const auto next_generation = m_generation + 1;
    m_swapchain = std::move(swapchain);
    m_queue = std::move(queue);
    m_device = std::move(device);
    m_observe_only = observe_only;
    m_generation = next_generation;
}

void XeFGBinding::clear() noexcept {
    m_swapchain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_generation = 0;
    m_observe_only = false;
}
