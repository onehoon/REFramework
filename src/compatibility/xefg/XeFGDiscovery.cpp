#include "XeFGDiscovery.hpp"

#include <wrl/client.h>

#include <spdlog/spdlog.h>

namespace {

constexpr int32_t kXefgSuccess = 0;

struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool device_available{false};
    bool valid{false};
};

const char* queue_relation_name(XeFGQueueRelation relation) {
    switch (relation) {
    case XeFGQueueRelation::SameComIdentity: return "same_com_identity";
    case XeFGQueueRelation::DistinctSameDevice: return "distinct_same_device";
    case XeFGQueueRelation::DeviceMismatch: return "device_mismatch";
    case XeFGQueueRelation::InitQueueUnavailable: return "init_queue_unavailable";
    case XeFGQueueRelation::PresentationQueueUnavailable: return "presentation_queue_unavailable";
    case XeFGQueueRelation::PresentationQueueNotDirect: return "presentation_queue_not_direct";
    default: return "unknown";
    }
}

const char* queue_type_name(D3D12_COMMAND_LIST_TYPE type) {
    switch (type) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT: return "direct";
    case D3D12_COMMAND_LIST_TYPE_BUNDLE: return "bundle";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE: return "compute";
    case D3D12_COMMAND_LIST_TYPE_COPY: return "copy";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE: return "video_decode";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS: return "video_process";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE: return "video_encode";
    default: return "unknown";
    }
}

QueueIdentitySnapshot capture_queue_identity(ID3D12CommandQueue* queue) {
    QueueIdentitySnapshot snapshot{};
    snapshot.queue = queue;
    if (queue == nullptr) {
        return snapshot;
    }

    Microsoft::WRL::ComPtr<IUnknown> queue_identity;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IUnknown> device_identity;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
        return snapshot;
    }

    snapshot.device_available = true;
    if (FAILED(queue->QueryInterface(IID_PPV_ARGS(&queue_identity)))
        || FAILED(device.As(&device_identity))) {
        return snapshot;
    }

    snapshot.com_identity = queue_identity.Get();
    snapshot.device_identity = device_identity.Get();
    snapshot.desc = queue->GetDesc();
    snapshot.valid = true;
    return snapshot;
}

void log_queue_identity(void* context, IDXGISwapChain3* swapchain, const QueueIdentitySnapshot& init, const QueueIdentitySnapshot& presentation, XeFGQueueRelation relation) {
    spdlog::info("[XeFG][QueueIdentity] context = 0x{:x}, swapchain = 0x{:x}, init_queue = 0x{:x}, init_identity = 0x{:x}, init_device_identity = 0x{:x}, init_type = {}, init_priority = {}, init_flags = 0x{:x}, init_node_mask = {}, presentation_queue = 0x{:x}, presentation_identity = 0x{:x}, presentation_device_identity = 0x{:x}, presentation_type = {}, presentation_priority = {}, presentation_flags = 0x{:x}, presentation_node_mask = {}, relation = {}",
        reinterpret_cast<uintptr_t>(context),
        reinterpret_cast<uintptr_t>(swapchain),
        reinterpret_cast<uintptr_t>(init.queue),
        reinterpret_cast<uintptr_t>(init.com_identity),
        reinterpret_cast<uintptr_t>(init.device_identity),
        queue_type_name(init.desc.Type),
        init.desc.Priority,
        static_cast<uint32_t>(init.desc.Flags),
        init.desc.NodeMask,
        reinterpret_cast<uintptr_t>(presentation.queue),
        reinterpret_cast<uintptr_t>(presentation.com_identity),
        reinterpret_cast<uintptr_t>(presentation.device_identity),
        queue_type_name(presentation.desc.Type),
        presentation.desc.Priority,
        static_cast<uint32_t>(presentation.desc.Flags),
        presentation.desc.NodeMask,
        queue_relation_name(relation));
}

XeFGQueueRelation classify_queue_relation(const QueueIdentitySnapshot& init, const QueueIdentitySnapshot& presentation) {
    if (!init.valid) {
        return XeFGQueueRelation::InitQueueUnavailable;
    }
    if (!presentation.valid) {
        return XeFGQueueRelation::PresentationQueueUnavailable;
    }
    if (init.device_identity != presentation.device_identity) {
        return XeFGQueueRelation::DeviceMismatch;
    }
    if (presentation.desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return XeFGQueueRelation::PresentationQueueNotDirect;
    }
    if (init.com_identity == presentation.com_identity) {
        return XeFGQueueRelation::SameComIdentity;
    }
    return XeFGQueueRelation::DistinctSameDevice;
}

const char* probe_reason_for(XeFGQueueRelation relation, const XeFGDiscovery::Observation& observation, const QueueIdentitySnapshot& presentation) {
    switch (relation) {
    case XeFGQueueRelation::SameComIdentity: return "same_com_identity";
    case XeFGQueueRelation::PresentationQueueUnavailable:
        return observation.presentation_queue == nullptr
            ? "presentation_queue_unavailable"
            : !presentation.device_available
                ? "presentation_queue_device_unavailable"
                : "presentation_queue_unavailable";
    case XeFGQueueRelation::DeviceMismatch: return "presentation_queue_device_mismatch";
    case XeFGQueueRelation::PresentationQueueNotDirect: return "presentation_queue_not_direct";
    default: return "presentation_queue_unavailable";
    }
}

} // namespace

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

XeFGBindingCandidateResult XeFGDiscovery::build_binding_candidate(const Observation& observation) {
    XeFGBindingCandidateResult result{};

    if (observation.init_result != kXefgSuccess) {
        result.reject_reason = "init_failed";
        return result;
    }
    if (!observation.factory_create_succeeded || observation.internal_swapchain == nullptr) {
        result.reject_reason = "no_candidate";
        return result;
    }
    if (observation.init_queue == nullptr) {
        result.reject_reason = "queue_device_unavailable";
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    if (FAILED(observation.internal_swapchain->QueryInterface(IID_PPV_ARGS(&swapchain)))) {
        result.reject_reason = "no_idxgi_swapchain3";
        return result;
    }

    HWND candidate_hwnd{};
    if (FAILED(swapchain->GetHwnd(&candidate_hwnd)) || candidate_hwnd != observation.hwnd) {
        result.reject_reason = "hwnd_mismatch";
        return result;
    }

    Microsoft::WRL::ComPtr<ID3D12Device4> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        result.reject_reason = "candidate_device_unavailable";
        return result;
    }

    Microsoft::WRL::ComPtr<IUnknown> candidate_device_identity;
    if (FAILED(device.As(&candidate_device_identity))) {
        result.reject_reason = "candidate_device_unavailable";
        return result;
    }

    const auto init_queue = capture_queue_identity(observation.init_queue);
    const auto presentation_queue = capture_queue_identity(observation.presentation_queue);
    const auto relation = classify_queue_relation(init_queue, presentation_queue);
    log_queue_identity(observation.context, swapchain.Get(), init_queue, presentation_queue, relation);

    if (!init_queue.valid) {
        result.reject_reason = "queue_device_unavailable";
        return result;
    }
    if (candidate_device_identity.Get() != init_queue.device_identity) {
        result.reject_reason = "device_mismatch";
        return result;
    }

    XeFGBindingCandidate candidate{};
    candidate.swapchain = swapchain;
    candidate.device = device;
    candidate.hwnd = observation.hwnd;
    candidate.relation = relation;

    if (relation == XeFGQueueRelation::DistinctSameDevice) {
        candidate.selected_queue = presentation_queue.queue;
        candidate.observe_only = false;
        result.bind_reason = "init_success";
        result.probe_reason = "distinct_same_device";
    } else {
        candidate.selected_queue = init_queue.queue;
        candidate.observe_only = true;
        result.bind_reason = "init_success_observe_only";
        result.probe_reason = probe_reason_for(relation, observation, presentation_queue);
    }

    if (!candidate.valid()) {
        result.reject_reason = "queue_device_unavailable";
        return result;
    }

    result.candidate = std::move(candidate);
    return result;
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
