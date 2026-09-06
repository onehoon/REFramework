#include <thread>
#include <future>
#include <unordered_set>
#include <stacktrace>
#include <algorithm>
#include <optional>
#include <string>
#include <wrl/client.h>

#include <spdlog/spdlog.h>
#include <utility/Thread.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include <utility/RTTI.hpp>
#include <utility/Scan.hpp>
#include <utility/ScopeGuard.hpp>

#include "REFramework.hpp"

#include "WindowFilter.hpp"

#include "D3D12Hook.hpp"

static D3D12Hook* g_d3d12_hook = nullptr;
thread_local bool g_inside_d3d12_hook = false;

namespace {

constexpr int32_t kXefgSuccess = 0;

enum class XefgQueueRelation {
    SameComIdentity,
    DistinctSameDevice,
    DeviceMismatch,
    InitQueueUnavailable,
    PresentationQueueUnavailable,
    PresentationQueueNotDirect,
};

struct XefgInitTransaction {
    void* context{};
    HWND hwnd{};
    ID3D12CommandQueue* init_queue{};
    ID3D12CommandQueue* presentation_queue{};
    IDXGIFactory2* factory{};
    IDXGISwapChain1* candidate{};
    bool factory_create_succeeded{ false };
    int32_t init_result{ -1 };
};

struct PendingXefgBinding {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> selected_queue{};
    HWND hwnd{};
    XefgQueueRelation relation{ XefgQueueRelation::InitQueueUnavailable };
    bool observe_only{ true };
    bool valid() const {
        return swapchain != nullptr && selected_queue != nullptr;
    }
};

const char* binding_change_reason(bool swapchain_changed, bool queue_changed, bool mode_changed) {
    const auto change_count = static_cast<int>(swapchain_changed)
        + static_cast<int>(queue_changed)
        + static_cast<int>(mode_changed);

    if (change_count > 1) {
        return "multiple_fields_changed";
    }
    if (swapchain_changed) {
        return "swapchain_changed";
    }
    if (queue_changed) {
        return "queue_changed";
    }
    if (mode_changed) {
        return "mode_changed";
    }
    return "identical";
}

void log_xefg_rebind(std::string_view stage, const char* reason, uint64_t generation, IDXGISwapChain3* old_swapchain, IDXGISwapChain3* new_swapchain, ID3D12CommandQueue* old_queue, ID3D12CommandQueue* new_queue, bool old_observe_only, bool new_observe_only) {
    spdlog::info("[XeFG][Rebind] stage = {}, reason = {}, generation = {}, old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, old_queue = 0x{:x}, new_queue = 0x{:x}, old_observe_only = {}, new_observe_only = {}",
        stage,
        reason,
        generation,
        reinterpret_cast<uintptr_t>(old_swapchain),
        reinterpret_cast<uintptr_t>(new_swapchain),
        reinterpret_cast<uintptr_t>(old_queue),
        reinterpret_cast<uintptr_t>(new_queue),
        old_observe_only,
        new_observe_only);
}

struct QueueIdentitySnapshot {
    ID3D12CommandQueue* queue{};
    void* com_identity{};
    void* device_identity{};
    D3D12_COMMAND_QUEUE_DESC desc{};
    bool device_available{ false };
    bool valid{ false };
};

std::mutex g_xefg_state_mutex{};
XefgInitTransaction g_xefg_transaction{};
std::optional<PendingXefgBinding> g_pending_xefg_binding{};
std::unique_ptr<VtableHook> g_xefg_factory_hook{};
std::unique_ptr<FunctionHook> g_xefg_init_hook{};
std::unique_ptr<FunctionHook> g_xefg_get_swapchain_hook{};

const auto g_diagnostic_start_time = std::chrono::steady_clock::now();

const char* queue_relation_name(XefgQueueRelation relation) {
    switch (relation) {
    case XefgQueueRelation::SameComIdentity: return "same_com_identity";
    case XefgQueueRelation::DistinctSameDevice: return "distinct_same_device";
    case XefgQueueRelation::DeviceMismatch: return "device_mismatch";
    case XefgQueueRelation::InitQueueUnavailable: return "init_queue_unavailable";
    case XefgQueueRelation::PresentationQueueUnavailable: return "presentation_queue_unavailable";
    case XefgQueueRelation::PresentationQueueNotDirect: return "presentation_queue_not_direct";
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

void log_xefg_queue_identity(void* context, IDXGISwapChain3* swapchain, const QueueIdentitySnapshot& init, const QueueIdentitySnapshot& presentation, XefgQueueRelation relation) {
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

struct SwapchainVtableSnapshot {
    void* object{};
    void** vtable{};
    void* present{};
    void* resize_buffers{};
    void* resize_target{};
    void* present1{};
    void* resize_buffers1{};
};

bool is_readable(const void* address, size_t size) {
    if (address == nullptr || size == 0) {
        return false;
    }

    auto current = reinterpret_cast<uintptr_t>(address);
    const auto end = current + size;
    if (end < current) {
        return false;
    }

    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &mbi, sizeof(mbi)) != sizeof(mbi)
            || mbi.State != MEM_COMMIT
            || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }

        const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= current) {
            return false;
        }

        current = (std::min)(region_end, end);
    }

    return true;
}

void* read_vtable_slot(void** vtable, size_t slot) {
    if (vtable == nullptr || !is_readable(vtable, (slot + 1) * sizeof(void*))) {
        return nullptr;
    }

    return vtable[slot];
}

std::string describe_address(void* address) {
    if (address == nullptr) {
        return "unknown";
    }

    try {
        const auto module = utility::get_module_within(address);
        if (!module) {
            return "unknown";
        }

        const auto path = utility::get_module_pathw(*module);
        if (!path) {
            return fmt::format("0x{:x}", reinterpret_cast<uintptr_t>(*module));
        }

        return fmt::format("0x{:x} [{}]", reinterpret_cast<uintptr_t>(*module), utility::narrow(*path));
    } catch (...) {
        return "unknown";
    }
}

const char* resize_kind_name(D3D12Hook::XefgResizeEventKind kind) {
    switch (kind) {
    case D3D12Hook::XefgResizeEventKind::ResizeTarget: return "ResizeTarget";
    case D3D12Hook::XefgResizeEventKind::ResizeBuffers: return "ResizeBuffers";
    case D3D12Hook::XefgResizeEventKind::ResizeBuffers1: return "ResizeBuffers1";
    default: return "None";
    }
}

std::optional<SwapchainVtableSnapshot> snapshot_swapchain(IUnknown* object) {
    if (object == nullptr) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&swapchain))) || swapchain == nullptr) {
        return std::nullopt;
    }

    if (!is_readable(swapchain.Get(), sizeof(void*))) {
        return std::nullopt;
    }

    auto vtable = *reinterpret_cast<void***>(swapchain.Get());
    if (!is_readable(vtable, sizeof(void*))) {
        return std::nullopt;
    }

    return SwapchainVtableSnapshot{
        .object = swapchain.Get(),
        .vtable = vtable,
        .present = read_vtable_slot(vtable, 8),
        .resize_buffers = read_vtable_slot(vtable, 13),
        .resize_target = read_vtable_slot(vtable, 14),
        .present1 = read_vtable_slot(vtable, 22),
        .resize_buffers1 = read_vtable_slot(vtable, 39),
    };
}

void log_swapchain_vtable(std::string_view prefix, const SwapchainVtableSnapshot& snapshot) {
    spdlog::info("{} object = 0x{:x}, vtable = 0x{:x}", prefix, reinterpret_cast<uintptr_t>(snapshot.object), reinterpret_cast<uintptr_t>(snapshot.vtable));
    spdlog::info("{} Present[8] = 0x{:x}, owner = {}", prefix, reinterpret_cast<uintptr_t>(snapshot.present), describe_address(snapshot.present));
    spdlog::info("{} ResizeBuffers[13] = 0x{:x}, owner = {}", prefix, reinterpret_cast<uintptr_t>(snapshot.resize_buffers), describe_address(snapshot.resize_buffers));
    spdlog::info("{} ResizeTarget[14] = 0x{:x}, owner = {}", prefix, reinterpret_cast<uintptr_t>(snapshot.resize_target), describe_address(snapshot.resize_target));
    spdlog::info("{} Present1[22] = 0x{:x}, owner = {}", prefix, reinterpret_cast<uintptr_t>(snapshot.present1), describe_address(snapshot.present1));
    spdlog::info("{} ResizeBuffers1[39] = 0x{:x}, owner = {}", prefix, reinterpret_cast<uintptr_t>(snapshot.resize_buffers1), describe_address(snapshot.resize_buffers1));
}

const char* format_name(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "unknown";
    }
}

const char* swap_effect_name(DXGI_SWAP_EFFECT effect) {
    switch (effect) {
    case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
    default: return "unknown";
    }
}

void log_discovery_snapshot(IUnknown* dummy_swapchain, void** swapchain_vtable, IDXGIFactory4* factory, void** factory_vtable, ID3D12CommandQueue* command_queue) {
    spdlog::info("[D3D12][Discovery] dummy_swapchain = 0x{:x}, dummy_vtable = 0x{:x}, factory = 0x{:x}, factory_vtable = 0x{:x}, command_queue = 0x{:x}, command_queue_offset = 0x{:x}",
        reinterpret_cast<uintptr_t>(dummy_swapchain),
        reinterpret_cast<uintptr_t>(swapchain_vtable),
        reinterpret_cast<uintptr_t>(factory),
        reinterpret_cast<uintptr_t>(factory_vtable),
        reinterpret_cast<uintptr_t>(command_queue),
        D3D12Hook::get_command_queue_offset_for_diagnostics());

    if (dummy_swapchain != nullptr) {
        if (const auto snapshot = snapshot_swapchain(dummy_swapchain)) {
            log_swapchain_vtable("[D3D12][Discovery]", *snapshot);
        } else {
            spdlog::info("[D3D12][Discovery] swapchain interface = unavailable");
        }
    } else if (swapchain_vtable != nullptr) {
        spdlog::info("[D3D12][Discovery] Present[8] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(read_vtable_slot(swapchain_vtable, 8)), describe_address(read_vtable_slot(swapchain_vtable, 8)));
        spdlog::info("[D3D12][Discovery] ResizeBuffers[13] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(read_vtable_slot(swapchain_vtable, 13)), describe_address(read_vtable_slot(swapchain_vtable, 13)));
        spdlog::info("[D3D12][Discovery] ResizeTarget[14] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(read_vtable_slot(swapchain_vtable, 14)), describe_address(read_vtable_slot(swapchain_vtable, 14)));
        spdlog::info("[D3D12][Discovery] Present1[22] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(read_vtable_slot(swapchain_vtable, 22)), describe_address(read_vtable_slot(swapchain_vtable, 22)));
        spdlog::info("[D3D12][Discovery] ResizeBuffers1[39] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(read_vtable_slot(swapchain_vtable, 39)), describe_address(read_vtable_slot(swapchain_vtable, 39)));
    }

    if (factory_vtable != nullptr) {
        const auto create_swapchain = read_vtable_slot(factory_vtable, 15);
        spdlog::info("[D3D12][Discovery] CreateSwapChainForHwnd[15] = 0x{:x}, owner = {}", reinterpret_cast<uintptr_t>(create_swapchain), describe_address(create_swapchain));
    }
}

} // namespace

D3D12Hook::~D3D12Hook() {
    unhook();
}

void D3D12Hook::install_xefg_api_hooks_if_available() {
    std::scoped_lock lock{g_xefg_state_mutex};

    const auto module = GetModuleHandleW(L"libxess_fg.dll");
    if (module == nullptr) {
        return;
    }

    const auto init_export = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    if (init_export != nullptr && g_xefg_init_hook == nullptr) {
        auto hook = std::make_unique<FunctionHook>(Address{reinterpret_cast<void*>(init_export)}, reinterpret_cast<void*>(&D3D12Hook::xefg_init_from_swapchain_desc));
        if (hook->create()) {
            g_xefg_init_hook = std::move(hook);
            spdlog::info("[XeFG][ApiHook] InitFromSwapChainDesc = 0x{:x}", reinterpret_cast<uintptr_t>(init_export));
        } else {
            spdlog::error("[XeFG][ApiHook] Failed to hook InitFromSwapChainDesc");
        }
    }

    const auto get_swapchain_export = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");
    if (get_swapchain_export != nullptr && g_xefg_get_swapchain_hook == nullptr) {
        auto hook = std::make_unique<FunctionHook>(Address{reinterpret_cast<void*>(get_swapchain_export)}, reinterpret_cast<void*>(&D3D12Hook::xefg_get_swapchain_ptr));
        if (hook->create()) {
            g_xefg_get_swapchain_hook = std::move(hook);
            spdlog::info("[XeFG][ApiHook] GetSwapChainPtr = 0x{:x}", reinterpret_cast<uintptr_t>(get_swapchain_export));
        } else {
            spdlog::error("[XeFG][ApiHook] Failed to hook GetSwapChainPtr");
        }
    }
}

int32_t D3D12Hook::xefg_init_from_swapchain_desc(void* context, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* swap_chain_desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, ID3D12CommandQueue* command_queue, IDXGIFactory2* factory, const void* init_params) {
    using XefgInitFn = int32_t (WINAPI*)(void*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, ID3D12CommandQueue*, IDXGIFactory2*, const void*);
    const auto original = reinterpret_cast<XefgInitFn>(g_xefg_init_hook->get_original());

    XefgInitTransaction transaction{};
    transaction.context = context;
    transaction.hwnd = hwnd;
    transaction.init_queue = command_queue;
    transaction.factory = factory;

    {
        std::scoped_lock lock{g_xefg_state_mutex};
        g_xefg_transaction = transaction;
        g_xefg_factory_hook.reset();

        if (factory != nullptr) {
            try {
                g_xefg_factory_hook = std::make_unique<VtableHook>(Address{factory});
                const auto hooked = g_xefg_factory_hook->hook_method(15, Address{reinterpret_cast<void*>(&D3D12Hook::create_xefg_swapchain)});
                if (!hooked) {
                    g_xefg_factory_hook.reset();
                }
            } catch (...) {
                g_xefg_factory_hook.reset();
            }
        }
    }

    const auto result = original(context, hwnd, swap_chain_desc, fullscreen_desc, command_queue, factory, init_params);

    {
        std::scoped_lock lock{g_xefg_state_mutex};
        g_xefg_transaction.init_result = result;
        spdlog::info("[XeFG][InitDesc] context = 0x{:x}, hwnd = 0x{:x}, queue = 0x{:x}, factory = 0x{:x}, width = {}, height = {}, format = {}, buffer_count = {}, flags = 0x{:x}, result = {}",
            reinterpret_cast<uintptr_t>(context),
            reinterpret_cast<uintptr_t>(hwnd),
            reinterpret_cast<uintptr_t>(command_queue),
            reinterpret_cast<uintptr_t>(factory),
            swap_chain_desc != nullptr ? swap_chain_desc->Width : 0,
            swap_chain_desc != nullptr ? swap_chain_desc->Height : 0,
            swap_chain_desc != nullptr ? swap_chain_desc->Format : DXGI_FORMAT_UNKNOWN,
            swap_chain_desc != nullptr ? swap_chain_desc->BufferCount : 0,
            swap_chain_desc != nullptr ? swap_chain_desc->Flags : 0,
            result);
        g_xefg_factory_hook.reset();
    }

    publish_xefg_candidate();
    return result;
}

int32_t D3D12Hook::xefg_get_swapchain_ptr(void* context, REFIID riid, void** swap_chain) {
    using XefgGetSwapchainFn = int32_t (WINAPI*)(void*, REFIID, void**);
    const auto original = reinterpret_cast<XefgGetSwapchainFn>(g_xefg_get_swapchain_hook->get_original());
    const auto result = original(context, riid, swap_chain);

    if (result == kXefgSuccess && swap_chain != nullptr && *swap_chain != nullptr) {
        std::scoped_lock lock{g_xefg_state_mutex};
        const auto internal_candidate = g_xefg_transaction.candidate;
        spdlog::info("[XeFG][PublicProxy] context = 0x{:x}, swapchain = 0x{:x}, internal_same = {}",
            reinterpret_cast<uintptr_t>(context),
            reinterpret_cast<uintptr_t>(*swap_chain),
            *swap_chain == internal_candidate);
    }

    return result;
}

HRESULT WINAPI D3D12Hook::create_xefg_swapchain(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
    std::unique_ptr<VtableHook>* factory_hook = &g_xefg_factory_hook;
    if (*factory_hook == nullptr) {
        return E_FAIL;
    }

    using CreateSwapchainFn = HRESULT (WINAPI*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    const auto original = (*factory_hook)->get_method<CreateSwapchainFn>(15);
    const auto result = original(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);

    if (SUCCEEDED(result) && swap_chain != nullptr && *swap_chain != nullptr) {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentation_queue;
        if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&presentation_queue)))) {
            g_xefg_transaction.presentation_queue = presentation_queue.Get();
        }

        g_xefg_transaction.candidate = *swap_chain;
        g_xefg_transaction.factory_create_succeeded = true;
        spdlog::info("[XeFG][InternalSwapchain] context = 0x{:x}, candidate = 0x{:x}, presentation_queue = 0x{:x}, provisional = true",
            reinterpret_cast<uintptr_t>(g_xefg_transaction.context),
            reinterpret_cast<uintptr_t>(*swap_chain),
            reinterpret_cast<uintptr_t>(g_xefg_transaction.presentation_queue));
    }

    return result;
}

void D3D12Hook::publish_xefg_candidate() {
    PendingXefgBinding pending{};
    const char* reject_reason = nullptr;
    const char* bind_reason = "init_success";
    const char* probe_reason = "distinct_same_device";

    {
        std::scoped_lock lock{g_xefg_state_mutex};
        const auto& transaction = g_xefg_transaction;

        if (transaction.init_result != kXefgSuccess) {
            reject_reason = "init_failed";
        } else if (!transaction.factory_create_succeeded || transaction.candidate == nullptr) {
            reject_reason = "no_candidate";
        } else if (transaction.init_queue == nullptr) {
            reject_reason = "queue_device_unavailable";
        } else {
            Microsoft::WRL::ComPtr<IDXGISwapChain3> candidate;
            Microsoft::WRL::ComPtr<ID3D12Device> candidate_device;
            Microsoft::WRL::ComPtr<IUnknown> candidate_identity;
            HWND candidate_hwnd{};

            if (FAILED(transaction.candidate->QueryInterface(IID_PPV_ARGS(&candidate)))) {
                reject_reason = "no_idxgi_swapchain3";
            } else if (FAILED(candidate->GetHwnd(&candidate_hwnd)) || candidate_hwnd != transaction.hwnd) {
                reject_reason = "hwnd_mismatch";
            } else if (FAILED(candidate->GetDevice(IID_PPV_ARGS(&candidate_device)))) {
                reject_reason = "candidate_device_unavailable";
            } else if (FAILED(candidate_device.As(&candidate_identity))) {
                reject_reason = "candidate_device_unavailable";
            } else {
                const auto init_queue = capture_queue_identity(transaction.init_queue);
                const auto presentation_queue = capture_queue_identity(transaction.presentation_queue);
                auto relation = XefgQueueRelation::InitQueueUnavailable;

                if (!init_queue.valid) {
                    relation = XefgQueueRelation::InitQueueUnavailable;
                } else if (!presentation_queue.valid) {
                    relation = XefgQueueRelation::PresentationQueueUnavailable;
                } else if (init_queue.device_identity != presentation_queue.device_identity) {
                    relation = XefgQueueRelation::DeviceMismatch;
                } else if (presentation_queue.desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    relation = XefgQueueRelation::PresentationQueueNotDirect;
                } else if (init_queue.com_identity == presentation_queue.com_identity) {
                    relation = XefgQueueRelation::SameComIdentity;
                } else {
                    relation = XefgQueueRelation::DistinctSameDevice;
                }

                log_xefg_queue_identity(transaction.context, candidate.Get(), init_queue, presentation_queue, relation);

                if (!init_queue.valid) {
                    reject_reason = "queue_device_unavailable";
                } else if (candidate_identity.Get() != init_queue.device_identity) {
                    reject_reason = "device_mismatch";
                } else if (relation == XefgQueueRelation::DistinctSameDevice) {
                    pending.swapchain = candidate;
                    pending.selected_queue = presentation_queue.queue;
                    pending.hwnd = transaction.hwnd;
                    pending.relation = relation;
                    pending.observe_only = false;
                } else {
                    // Preserve liveness and the original Present/Present1 calls, but
                    // do not submit overlay work through an unproven XeFG queue path.
                    pending.swapchain = candidate;
                    pending.selected_queue = init_queue.queue;
                    pending.hwnd = transaction.hwnd;
                    pending.relation = relation;
                    pending.observe_only = true;
                    bind_reason = "init_success_observe_only";
                    switch (relation) {
                    case XefgQueueRelation::SameComIdentity:
                        probe_reason = "same_com_identity";
                        break;
                    case XefgQueueRelation::PresentationQueueUnavailable:
                        probe_reason = transaction.presentation_queue == nullptr
                            ? "presentation_queue_unavailable"
                            : !presentation_queue.device_available
                                ? "presentation_queue_device_unavailable"
                                : "presentation_queue_unavailable";
                        break;
                    case XefgQueueRelation::DeviceMismatch:
                        probe_reason = "presentation_queue_device_mismatch";
                        break;
                    case XefgQueueRelation::PresentationQueueNotDirect:
                        probe_reason = "presentation_queue_not_direct";
                        break;
                    default:
                        probe_reason = "presentation_queue_unavailable";
                        break;
                    }
                }
            }
        }

        if (reject_reason != nullptr) {
            spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = {}",
                reinterpret_cast<uintptr_t>(transaction.candidate), reject_reason);
        } else {
            spdlog::info("[XeFG][Bind] candidate = 0x{:x}, accepted = true, reason = {}",
                reinterpret_cast<uintptr_t>(pending.swapchain.Get()), bind_reason);
            spdlog::info("[XeFG][P2.1Probe] mode = {}, selected_queue = 0x{:x}, render_callbacks = {}, reason = {}",
                pending.observe_only ? (pending.relation == XefgQueueRelation::SameComIdentity ? "observe_only_same_queue" : "observe_only_invalid_presentation_queue") : "presentation_queue_render",
                reinterpret_cast<uintptr_t>(pending.selected_queue.Get()),
                !pending.observe_only,
                probe_reason);
        }
    }

    if (reject_reason != nullptr) {
        return;
    }

    if (g_framework != nullptr) {
        std::unique_lock<std::recursive_mutex> framework_lock{g_framework->get_hook_monitor_mutex()};

        // Hook-monitor recovery destroys and replaces D3D12Hook under this same
        // mutex. Read the current object only after acquiring it so a XeFG init
        // cannot bind through a pointer retained from before that replacement.
        if (auto* hook = g_d3d12_hook; hook != nullptr) {
            const auto has_active_xefg = hook->is_hooked()
                && hook->get_swap_chain() != nullptr
                && hook->get_swapchain_source() == SwapchainSource::XeFGInternal;
            if (has_active_xefg) {
                const auto swapchain_changed = hook->get_swap_chain() != pending.swapchain.Get();
                const auto queue_changed = hook->get_command_queue() != pending.selected_queue.Get();
                const auto mode_changed = hook->is_xefg_observe_only() != pending.observe_only;
                const auto changed = swapchain_changed || queue_changed || mode_changed;
                const auto reason = binding_change_reason(swapchain_changed, queue_changed, mode_changed);

                spdlog::info("[XeFG][BindingGate] action = {}, reason = {}, old_swapchain = 0x{:x}, new_swapchain = 0x{:x}, old_queue = 0x{:x}, new_queue = 0x{:x}, old_observe_only = {}, new_observe_only = {}",
                    changed ? "rebind" : "unchanged",
                    reason,
                    reinterpret_cast<uintptr_t>(hook->get_swap_chain()),
                    reinterpret_cast<uintptr_t>(pending.swapchain.Get()),
                    reinterpret_cast<uintptr_t>(hook->get_command_queue()),
                    reinterpret_cast<uintptr_t>(pending.selected_queue.Get()),
                    hook->is_xefg_observe_only(),
                    pending.observe_only);

                if (changed && !hook->replace_xefg_binding(pending.swapchain.Get(), pending.selected_queue.Get(), pending.observe_only, reason)) {
                    spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = rebind_failed",
                        reinterpret_cast<uintptr_t>(pending.swapchain.Get()));
                }
                return;
            }

            const auto replacing_active_non_xefg = hook->is_hooked()
                && hook->get_swap_chain() != nullptr
                && hook->get_swapchain_source() != SwapchainSource::XeFGInternal;
            if (replacing_active_non_xefg) {
                spdlog::info("[XeFG][Bind] resetting active D3D12 renderer before XeFG bind");
                g_framework->on_reset();
            }

            if (!hook->bind_external_swapchain(pending.swapchain.Get(), pending.selected_queue.Get(), SwapchainSource::XeFGInternal, pending.observe_only)) {
                spdlog::warn("[XeFG][Bind] candidate = 0x{:x}, accepted = false, reason = external_bind_failed",
                    reinterpret_cast<uintptr_t>(pending.swapchain.Get()));
            }
            return;
        }

        // Publish before releasing the lifecycle mutex. D3D12Hook::hook() runs
        // under the same mutex, so a newly created hook cannot consume an empty
        // slot and then miss this capture-before-hook XeFG binding.
        std::scoped_lock state_lock{g_xefg_state_mutex};
        g_pending_xefg_binding = pending;
        return;
    }

    // Constructor-time fallback before REFramework publishes its lifecycle mutex.
    std::scoped_lock lock{g_xefg_state_mutex};
    g_pending_xefg_binding = pending;
}

bool D3D12Hook::consume_pending_xefg_binding(D3D12Hook& hook) {
    std::optional<PendingXefgBinding> pending{};
    {
        std::scoped_lock lock{g_xefg_state_mutex};
        pending = g_pending_xefg_binding;
        g_pending_xefg_binding.reset();
    }

    return pending.has_value() && hook.bind_external_swapchain(pending->swapchain.Get(), pending->selected_queue.Get(), SwapchainSource::XeFGInternal, pending->observe_only);
}

bool D3D12Hook::external_binding_matches(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue, SwapchainSource source, bool xefg_observe_only) const {
    const auto normalized_observe_only = source == SwapchainSource::XeFGInternal && xefg_observe_only;

    if (!m_hooked
        || m_swapchain_hook == nullptr
        || m_swapchain_source != source
        || m_swap_chain != swapchain
        || m_command_queue != command_queue) {
        return false;
    }

    return source != SwapchainSource::XeFGInternal || m_xefg_p21_observe_only == normalized_observe_only;
}

bool D3D12Hook::replace_xefg_binding(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue, bool observe_only, const char* reason) {
    if (swapchain == nullptr
        || command_queue == nullptr
        || !m_hooked
        || m_swapchain_source != SwapchainSource::XeFGInternal
        || m_swap_chain == nullptr
        || m_swapchain_hook == nullptr) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> next_swapchain = swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_queue = command_queue;
    Microsoft::WRL::ComPtr<ID3D12Device4> next_device;
    if (FAILED(next_swapchain->GetDevice(IID_PPV_ARGS(&next_device)))) {
        log_xefg_rebind("failed", "new_device_unavailable", m_xefg_binding_generation, m_swap_chain, swapchain, m_command_queue, command_queue, m_xefg_p21_observe_only, observe_only);
        return false;
    }

    auto* const old_swapchain = m_swap_chain;
    auto* const old_queue = m_command_queue;
    const auto old_observe_only = m_xefg_p21_observe_only;
    const auto same_swapchain = old_swapchain == swapchain;

    log_xefg_rebind("begin", reason, m_xefg_binding_generation, old_swapchain, swapchain, old_queue, command_queue, old_observe_only, observe_only);

    if (same_swapchain) {
        spdlog::info("[XeFG][Rebind] stage = old_renderer_reset, reason = {}, generation = {}", reason, m_xefg_binding_generation);
        g_framework->on_reset();

        m_xefg_bound_queue = std::move(next_queue);
        m_xefg_bound_device = std::move(next_device);
        m_swap_chain = m_xefg_bound_swapchain.Get();
        m_command_queue = m_xefg_bound_queue.Get();
        m_device = m_xefg_bound_device.Get();
        m_xefg_p21_observe_only = observe_only;
        m_xefg_p21_render_boundary_logged = false;
        ++m_xefg_binding_generation;

        log_xefg_rebind("same_object_updated", reason, m_xefg_binding_generation, old_swapchain, m_swap_chain, old_queue, m_command_queue, old_observe_only, m_xefg_p21_observe_only);
        return true;
    }

    std::unique_ptr<VtableHook> next_hook;
    try {
        next_hook = std::make_unique<VtableHook>(Address{next_swapchain.Get()});
        const auto present_ok = next_hook->hook_method(8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
        const auto present1_ok = next_hook->hook_method(22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
        const auto resize_buffers_ok = next_hook->hook_method(13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
        const auto resize_target_ok = next_hook->hook_method(14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});
        const auto resize_buffers1_ok = next_hook->hook_method(39, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});
        if (!(present_ok && present1_ok && resize_buffers_ok && resize_target_ok && resize_buffers1_ok)) {
            log_xefg_rebind("failed", "new_hook_method_failed", m_xefg_binding_generation, old_swapchain, swapchain, old_queue, command_queue, old_observe_only, observe_only);
            return false;
        }
    } catch (...) {
        log_xefg_rebind("failed", "new_hook_create_failed", m_xefg_binding_generation, old_swapchain, swapchain, old_queue, command_queue, old_observe_only, observe_only);
        return false;
    }

    log_xefg_rebind("new_hook_prepared", reason, m_xefg_binding_generation, old_swapchain, swapchain, old_queue, command_queue, old_observe_only, observe_only);
    spdlog::info("[XeFG][Rebind] stage = old_renderer_reset, reason = {}, generation = {}", reason, m_xefg_binding_generation);
    g_framework->on_reset();

    m_present_hook.reset();
    m_swapchain_hook.reset();
    log_xefg_rebind("old_hook_removed", reason, m_xefg_binding_generation, old_swapchain, swapchain, old_queue, command_queue, old_observe_only, observe_only);

    m_xefg_bound_swapchain = std::move(next_swapchain);
    m_xefg_bound_queue = std::move(next_queue);
    m_xefg_bound_device = std::move(next_device);
    m_swap_chain = m_xefg_bound_swapchain.Get();
    m_command_queue = m_xefg_bound_queue.Get();
    m_device = m_xefg_bound_device.Get();
    m_swapchain_hook = std::move(next_hook);
    m_swapchain_source = SwapchainSource::XeFGInternal;
    m_xefg_p21_observe_only = observe_only;
    m_xefg_p21_render_boundary_logged = false;
    m_is_phase_1 = false;
    m_hooked = true;
    ++m_xefg_binding_generation;

    log_xefg_rebind("new_binding_committed", reason, m_xefg_binding_generation, old_swapchain, m_swap_chain, old_queue, m_command_queue, old_observe_only, m_xefg_p21_observe_only);
    return true;
}

bool D3D12Hook::bind_external_swapchain(IDXGISwapChain3* swapchain, ID3D12CommandQueue* command_queue, SwapchainSource source, bool xefg_p21_observe_only) {
    if (swapchain == nullptr || command_queue == nullptr) {
        return false;
    }

    if (external_binding_matches(swapchain, command_queue, source, xefg_p21_observe_only)) {
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D12Device4> device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&device)))) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> next_xefg_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> next_xefg_queue;
    Microsoft::WRL::ComPtr<ID3D12Device4> next_xefg_device;
    if (source == SwapchainSource::XeFGInternal) {
        next_xefg_swapchain = swapchain;
        next_xefg_queue = command_queue;
        next_xefg_device = device;
    }

    // Existing XeFG ownership keeps the old instance alive through hook removal.
    m_present_hook.reset();
    m_swapchain_hook.reset();
    m_xefg_bound_swapchain.Reset();
    m_xefg_bound_queue.Reset();
    m_xefg_bound_device.Reset();

    if (source == SwapchainSource::XeFGInternal) {
        m_xefg_bound_swapchain = std::move(next_xefg_swapchain);
        m_xefg_bound_queue = std::move(next_xefg_queue);
        m_xefg_bound_device = std::move(next_xefg_device);
        m_swap_chain = m_xefg_bound_swapchain.Get();
        m_command_queue = m_xefg_bound_queue.Get();
        m_device = m_xefg_bound_device.Get();
    } else {
        m_swap_chain = swapchain;
        m_command_queue = command_queue;
        m_device = device.Get();
    }
    m_swapchain_source = source;
    m_xefg_p21_observe_only = source == SwapchainSource::XeFGInternal && xefg_p21_observe_only;
    m_xefg_p21_render_boundary_logged = false;
    m_is_phase_1 = false;

    m_swapchain_hook = std::make_unique<VtableHook>(Address{swapchain});
    m_swapchain_hook->hook_method(8, Address{reinterpret_cast<void*>(&D3D12Hook::present)});
    m_swapchain_hook->hook_method(22, Address{reinterpret_cast<void*>(&D3D12Hook::present1)});
    m_swapchain_hook->hook_method(13, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers)});
    m_swapchain_hook->hook_method(14, Address{reinterpret_cast<void*>(&D3D12Hook::resize_target)});

    void* resize_buffers1_original = nullptr;
    if (source == SwapchainSource::XeFGInternal) {
        m_swapchain_hook->hook_method(39, Address{reinterpret_cast<void*>(&D3D12Hook::resize_buffers1)});
        resize_buffers1_original = reinterpret_cast<void*>(m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers1)*>(39));
    }

    m_hooked = true;
    m_xefg_binding_generation = source == SwapchainSource::XeFGInternal ? 1 : 0;

    spdlog::info("[D3D12][ExternalBind] source = {}, swapchain = 0x{:x}, queue = 0x{:x}, device = 0x{:x}, Present[8].original = 0x{:x}, Present1[22].original = 0x{:x}, ResizeBuffers1[39].original = 0x{:x}",
        source == SwapchainSource::XeFGInternal ? "xefg_internal" : "native",
        reinterpret_cast<uintptr_t>(swapchain),
        reinterpret_cast<uintptr_t>(command_queue),
        reinterpret_cast<uintptr_t>(m_device),
        reinterpret_cast<uintptr_t>(m_swapchain_hook->get_method(8).ptr()),
        reinterpret_cast<uintptr_t>(m_swapchain_hook->get_method(22).ptr()),
        reinterpret_cast<uintptr_t>(resize_buffers1_original));

    return true;
}

void D3D12Hook::mark_xefg_probe_pending() noexcept {
    s_xefg_module_loaded.store(true, std::memory_order_release);

    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_xefg_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);

    s_xefg_probe_pending.store(true, std::memory_order_release);
}

void D3D12Hook::notify_xefg_module_loaded(HMODULE module, std::wstring_view base_name, std::wstring_view full_path) {
    s_xefg_module_loaded.store(true, std::memory_order_relaxed);

    const auto observed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_diagnostic_start_time).count();
    int64_t expected = -1;
    s_xefg_first_seen_ms.compare_exchange_strong(expected, observed_ms, std::memory_order_relaxed);

    spdlog::info("[XeFG][Module] name = {}, base = 0x{:x}, full_path = {}, first_seen_ms = {}",
        utility::narrow(std::wstring{base_name}), reinterpret_cast<uintptr_t>(module), utility::narrow(std::wstring{full_path}), s_xefg_first_seen_ms.load(std::memory_order_relaxed));

    const auto init_from_swap_chain = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChain");
    const auto init_from_swap_chain_desc = GetProcAddress(module, "xefgSwapChainD3D12InitFromSwapChainDesc");
    const auto get_swap_chain_ptr = GetProcAddress(module, "xefgSwapChainD3D12GetSwapChainPtr");

    spdlog::info("[XeFG][Exports] InitFromSwapChain = 0x{:x} / {}, InitFromSwapChainDesc = 0x{:x} / {}, GetSwapChainPtr = 0x{:x} / {}",
        reinterpret_cast<uintptr_t>(init_from_swap_chain), init_from_swap_chain != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(init_from_swap_chain_desc), init_from_swap_chain_desc != nullptr ? "present" : "missing",
        reinterpret_cast<uintptr_t>(get_swap_chain_ptr), get_swap_chain_ptr != nullptr ? "present" : "missing");

    install_xefg_api_hooks_if_available();
}

void D3D12Hook::process_pending_xefg_probe() {
    if (!s_xefg_probe_pending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    const auto module = GetModuleHandleW(L"libxess_fg.dll");
    if (module == nullptr) {
        return;
    }

    wchar_t path[MAX_PATH]{};
    const auto path_length = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    notify_xefg_module_loaded(module, L"libxess_fg.dll", std::wstring_view{path, path_length});
}

int64_t D3D12Hook::get_last_present_age_ms() const {
    if (m_last_present_entry_time.time_since_epoch().count() == 0) {
        return -1;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_last_present_entry_time).count();
}

void D3D12Hook::log_hook_monitor_snapshot(std::string_view event) const {
    spdlog::info("[D3D12][HookMonitor] event = {}, is_hooked = {}, is_phase_1 = {}, inside_present = {}, active_swapchain = 0x{:x}, active_device = 0x{:x}, active_command_queue = 0x{:x}, present_entry_count = {}, xefg_module_loaded = {}, last_present_entry_age_ms = {}",
        event,
        m_hooked,
        m_is_phase_1,
        m_inside_present,
        reinterpret_cast<uintptr_t>(m_swap_chain),
        reinterpret_cast<uintptr_t>(m_device),
        reinterpret_cast<uintptr_t>(m_command_queue),
        m_present_entry_count.load(std::memory_order_relaxed),
        is_xefg_module_loaded(),
        get_last_present_age_ms());
}

void* D3D12Hook::Streamline::link_swapchain_to_cmd_queue(void* rcx, void* rdx, void* r8, void* r9) {
    if (g_inside_d3d12_hook) {
        spdlog::info("[Streamline] linkSwapchainToCmdQueue: {:x} (inside D3D12 hook)", (uintptr_t)_ReturnAddress());

        auto& hook = D3D12Hook::s_streamline.link_swapchain_to_cmd_queue_hook;
        return hook->get_original<decltype(link_swapchain_to_cmd_queue)>()(rcx, rdx, r8, r9);
    }

    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("[Streamline] linkSwapchainToCmdQueue: {:x}", (uintptr_t)_ReturnAddress());

    bool hook_was_nullptr = g_d3d12_hook == nullptr;

    if (g_d3d12_hook != nullptr) {
        g_framework->on_reset(); // Needed to prevent a crash due to resources hanging around
        g_d3d12_hook->unhook(); // Removes all vtable hooks
    }

    auto& hook = D3D12Hook::s_streamline.link_swapchain_to_cmd_queue_hook;
    const auto result = hook->get_original<decltype(link_swapchain_to_cmd_queue)>()(rcx, rdx, r8, r9);

    // Re-hooks present after the above function creates the swapchain
    // This allows the hook to immediately still function
    // rather than waiting on the hook monitor to notice the hook isn't working
    if (!hook_was_nullptr) {
        g_framework->hook_d3d12();
    }

    return result;
}

HRESULT WINAPI D3D12Hook::create_swapchain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain1** swap_chain) {
    auto create_swap_chain_fn = s_create_swapchain_hook->get_original<decltype(D3D12Hook::create_swapchain)*>();

    if (g_inside_d3d12_hook) {
        spdlog::info("create_swapchain (inside D3D12 hook)");
        return create_swap_chain_fn(factory, device, hwnd, desc, p_fullscreen_desc, p_restrict_to_output, swap_chain);
    }

    spdlog::info("create_swapchain called");

    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    bool hook_was_nullptr = g_d3d12_hook == nullptr;

    if (g_d3d12_hook != nullptr && g_framework->get_d3d12_hook() != nullptr) {
        spdlog::info("[D3D12][HookLifecycle] action = unhook, reason = swapchain_reset_recreate");
        g_framework->on_reset(); // Needed to prevent a crash due to resources hanging around
        g_d3d12_hook->unhook(); // Removes all vtable hooks
    }

    const auto result = create_swap_chain_fn(factory, device, hwnd, desc, p_fullscreen_desc, p_restrict_to_output, swap_chain);

    if (SUCCEEDED(result) && swap_chain != nullptr && *swap_chain != nullptr) {
        static std::atomic<uint64_t> candidate_sequence{0};
        const auto sequence = candidate_sequence.fetch_add(1, std::memory_order_relaxed) + 1;

        std::string type_name = "unknown";
        try {
            const auto type_info = utility::rtti::get_type_info(*swap_chain);
            if (type_info != nullptr && type_info->name() != nullptr) {
                type_name = type_info->name();
            }
        } catch (...) {
            type_name = "unknown";
        }

        spdlog::info("[D3D12][SwapchainCandidate] sequence = {}, swapchain = 0x{:x}, factory = 0x{:x}, device_or_queue_arg = 0x{:x}, hwnd = 0x{:x}, width = {}, height = {}, format = {} ({}), buffer_count = {}, swap_effect = {} ({}), flags = 0x{:x}, type_name = {}, xefg_module_loaded = {}",
            sequence,
            reinterpret_cast<uintptr_t>(*swap_chain),
            reinterpret_cast<uintptr_t>(factory),
            reinterpret_cast<uintptr_t>(device),
            reinterpret_cast<uintptr_t>(hwnd),
            desc != nullptr ? desc->Width : 0,
            desc != nullptr ? desc->Height : 0,
            desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN,
            desc != nullptr ? format_name(desc->Format) : "unknown",
            desc != nullptr ? desc->BufferCount : 0,
            desc != nullptr ? desc->SwapEffect : DXGI_SWAP_EFFECT_DISCARD,
            desc != nullptr ? swap_effect_name(desc->SwapEffect) : "unknown",
            desc != nullptr ? desc->Flags : 0,
            type_name,
            D3D12Hook::is_xefg_module_loaded());

        if (const auto snapshot = snapshot_swapchain(*swap_chain)) {
            log_swapchain_vtable("[D3D12][SwapchainCandidate]", *snapshot);
        } else {
            spdlog::info("[D3D12][SwapchainCandidate] swapchain interface = unavailable");
        }

        if (type_name.find("interposer::DXGISwapChain") != std::string::npos) {
            spdlog::info("[D3D12][SwapchainCandidate] classification = streamline_interposer");
        } else if (type_name.find("FrameInterpolationSwapChain") != std::string::npos) {
            spdlog::info("[D3D12][SwapchainCandidate] classification = frame_interpolation_swapchain");
        } else {
            spdlog::info("[D3D12][SwapchainCandidate] classification = unclassified");
        }
    }

    // rather than waiting on the hook monitor to notice the hook isn't working
    if (!hook_was_nullptr) {
        g_framework->hook_d3d12();
    }

    return result;
}

void D3D12Hook::hook_streamline(HMODULE dlssg_module) try {
    if (D3D12Hook::s_streamline.setup) {
        return;
    }

    std::scoped_lock _{D3D12Hook::s_streamline.hook_mutex};

    if (D3D12Hook::s_streamline.setup) {
        return;
    }

    spdlog::info("[Streamline] Hooking Streamline");

    if (dlssg_module == nullptr) {
        dlssg_module = GetModuleHandleW(L"sl.dlss_g.dll");
    }

    if (dlssg_module == nullptr) {
        spdlog::error("[Streamline] Failed to get sl.dlss_g.dll module handle");
        return;
    }

    const auto str = utility::scan_string(dlssg_module, "linkSwapchainToCmdQueue");

    if (!str) {
        spdlog::error("[Streamline] Failed to find linkSwapchainToCmdQueue");
        return;
    }

    const auto str_ref = utility::scan_displacement_reference(dlssg_module, *str);

    if (!str_ref) {
        spdlog::error("[Streamline] Failed to find linkSwapchainToCmdQueue reference");
        return;
    }

    const auto fn = utility::find_function_start_with_call(*str_ref);

    if (!fn) {
        spdlog::error("[Streamline] Failed to find linkSwapchainToCmdQueue function");
        return;
    }

    D3D12Hook::s_streamline.link_swapchain_to_cmd_queue_hook = std::make_unique<FunctionHook>(*fn, (uintptr_t)&Streamline::link_swapchain_to_cmd_queue);

    if (D3D12Hook::s_streamline.link_swapchain_to_cmd_queue_hook->create()) {
        spdlog::info("[Streamline] Hooked linkSwapchainToCmdQueue");
    } else {
        spdlog::error("[Streamline] Failed to hook linkSwapchainToCmdQueue");
    }

    D3D12Hook::s_streamline.setup = true;
} catch(...) {
    spdlog::error("[Streamline] Failed to hook Streamline");
}

bool D3D12Hook::hook() {
    spdlog::info("Hooking D3D12");
    spdlog::info("[D3D12][HookLifecycle] action = hook, reason = initial_or_reinitialize");

    install_xefg_api_hooks_if_available();

    if (!is_xefg_module_loaded()) {
        if (const auto xefg_module = GetModuleHandleW(L"libxess_fg.dll")) {
            wchar_t path[MAX_PATH]{};
            const auto path_length = GetModuleFileNameW(xefg_module, path, ARRAYSIZE(path));
            notify_xefg_module_loaded(xefg_module, L"libxess_fg.dll", std::wstring_view{path, path_length});
        }
    }

    g_d3d12_hook = this;
    g_inside_d3d12_hook = true;

    utility::ScopeGuard guard{[]() {
        g_inside_d3d12_hook = false;
    }};

    if (consume_pending_xefg_binding(*this)) {
        spdlog::info("Hooked DirectX 12 through pending XeFG binding");
        return true;
    }

    if (s_command_queue_offset != 0 && s_swapchain_vtable != nullptr && s_factory_vtable != nullptr) {
        spdlog::info("Reinitializing D3D12Hook via known pointers");

        try {
            hook_impl();
        } catch (const std::exception& e) {
            spdlog::error("Failed to initialize hooks: {}", e.what());
            m_hooked = false;
        }

        return m_hooked;
    }

    IDXGISwapChain1* swap_chain1{ nullptr };
    IDXGISwapChain3* swap_chain{ nullptr };
    ID3D12Device* device{ nullptr };

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc1;

    ZeroMemory(&swap_chain_desc1, sizeof(swap_chain_desc1));

    swap_chain_desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap_chain_desc1.BufferCount = 2;
    swap_chain_desc1.SampleDesc.Count = 1;
    swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swap_chain_desc1.Width = 1;
    swap_chain_desc1.Height = 1;

    // Manually get D3D12CreateDevice export because the user may be running Windows 7
    const auto d3d12_module = LoadLibraryA("d3d12.dll");
    if (d3d12_module == nullptr) {
        spdlog::error("Failed to load d3d12.dll");
        return false;
    }

    auto d3d12_create_device = (decltype(D3D12CreateDevice)*)GetProcAddress(d3d12_module, "D3D12CreateDevice");
    if (d3d12_create_device == nullptr) {
        spdlog::error("Failed to get D3D12CreateDevice export");
        return false;
    }

    spdlog::info("Creating dummy device");

    // Get the original on-disk bytes of the D3D12CreateDevice export
    const auto original_bytes = utility::get_original_bytes(d3d12_create_device);

    // Temporarily unhook D3D12CreateDevice
    // it allows compatibility with ReShade and other overlays that hook it
    // this is just a dummy device anyways, we don't want the other overlays to be able to use it
    if (original_bytes) {
        spdlog::info("D3D12CreateDevice appears to be hooked, temporarily unhooking");

        std::vector<uint8_t> hooked_bytes(original_bytes->size());
        memcpy(hooked_bytes.data(), d3d12_create_device, original_bytes->size());

        ProtectionOverride protection_override{ d3d12_create_device, original_bytes->size(), PAGE_EXECUTE_READWRITE };
        memcpy(d3d12_create_device, original_bytes->data(), original_bytes->size());
        
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
            return false;
        }

        spdlog::info("Restoring hooked bytes for D3D12CreateDevice");
        memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
    } else { // D3D12CreateDevice is not hooked
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            return false;
        }
    }

    spdlog::info("Dummy device: {:x}", (uintptr_t)device);

    // Manually get CreateDXGIFactory export because the user may be running Windows 7
    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("Failed to load dxgi.dll");
        return false;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");

    if (create_dxgi_factory == nullptr) {
        spdlog::error("Failed to get CreateDXGIFactory export");
        return false;
    }

    spdlog::info("Creating dummy DXGI factory");

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory)))) {
        spdlog::error("Failed to create D3D12 Dummy DXGI Factory");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = 0;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;

    spdlog::info("Creating dummy command queue");

    ID3D12CommandQueue* command_queue{ nullptr };
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Queue");
        return false;
    }

    spdlog::info("Creating dummy swapchain");

    // used in CreateSwapChainForHwnd fallback
    HWND hwnd = 0;
    WNDCLASSEX wc{};

    auto init_dummy_window = [&]() {
        // fallback to CreateSwapChainForHwnd
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = TEXT("REFRAMEWORK_DX12_DUMMY");
        wc.hIconSm = NULL;

        ::RegisterClassEx(&wc);

        hwnd = ::CreateWindow(wc.lpszClassName, TEXT("REF DX Dummy Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        swap_chain_desc1.BufferCount = 3;
        swap_chain_desc1.Width = 0;
        swap_chain_desc1.Height = 0;
        swap_chain_desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc1.SampleDesc.Count = 1;
        swap_chain_desc1.SampleDesc.Quality = 0;
        swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swap_chain_desc1.Scaling = DXGI_SCALING_STRETCH;
        swap_chain_desc1.Stereo = FALSE;
    };

    std::vector<std::function<bool ()>> swapchain_attempts{
        // we call CreateSwapChainForComposition instead of CreateSwapChainForHwnd
        // because some overlays will have hooks on CreateSwapChainForHwnd
        // and all we're doing is creating a dummy swapchain
        // we don't want to screw up the overlay
        [&]() {
            return !FAILED(factory->CreateSwapChainForComposition(command_queue, &swap_chain_desc1, nullptr, &swap_chain1));
        },
        [&]() {
            init_dummy_window();

            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, hwnd, &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
        [&]() {
            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, GetDesktopWindow(), &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
    };

    bool any_succeed = false;

    for (auto i = 0; i < swapchain_attempts.size(); i++) {
        auto& attempt = swapchain_attempts[i];
        
        try {
            spdlog::info("Trying swapchain attempt {}", i);

            if (attempt()) {
                spdlog::info("Created dummy swapchain on attempt {}", i);
                any_succeed = true;
                break;
            }
        } catch (std::exception& e) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: {}", i, e.what());
        } catch(...) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: unknown exception", i);
        }

        spdlog::error("Attempt {} failed", i);
    }

    if (!any_succeed) {
        spdlog::error("Failed to create D3D12 Dummy Swap Chain");

        if (hwnd) {
            ::DestroyWindow(hwnd);
        }

        if (wc.lpszClassName != nullptr) {
            ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        }

        return false;
    }

    spdlog::info("Querying dummy swapchain");

    if (FAILED(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)))) {
        spdlog::error("Failed to retrieve D3D12 DXGI SwapChain");
        return false;
    }

    try {
        const auto ti = utility::rtti::get_type_info(swap_chain1);
        const auto swapchain_classname = ti != nullptr && ti->name() != nullptr ? std::string_view{ti->name()} : "unknown";
        const auto raw_name = ti != nullptr && ti->raw_name() != nullptr ? std::string_view{ti->raw_name()} : "unknown";

        spdlog::info("Swapchain type info: {}", swapchain_classname);
        spdlog::info("Swapchain raw type info: {}", raw_name);
        
        if (swapchain_classname.contains("interposer::DXGISwapChain")) { // DLSS3
            spdlog::info("Found Streamline (DLSSFG) swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
            m_using_frame_generation_swapchain = true;
        } else if (swapchain_classname.contains("FrameInterpolationSwapChain")) { // FSR3
            spdlog::info("Found FSR3 swapchain during dummy initialization: {:x}", (uintptr_t)swap_chain1);
            m_using_frame_generation_swapchain = true;
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to get type info: {}", e.what());
    } catch (...) {
        spdlog::error("Failed to get type info: unknown exception");
    }


    spdlog::info("Finding command queue offset");

    s_command_queue_offset = 0;

    // Find the command queue offset in the swapchain
    for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
        const auto base = (uintptr_t)swap_chain1 + i;

        // reached the end
        if (IsBadReadPtr((void*)base, sizeof(void*))) {
            break;
        }

        auto data = *(ID3D12CommandQueue**)base;

        if (data == command_queue) {
            s_command_queue_offset = i;
            spdlog::info("Found command queue offset: {:x}", i);
            break;
        }
    }

    auto target_swapchain = swap_chain;

    // Scan throughout the swapchain for a valid pointer to scan through
    // this is usually only necessary for Proton
    if (s_command_queue_offset == 0) {
        bool should_break = false;

        for (auto base = 0; base < 512 * sizeof(void*); base += sizeof(void*)) {
            const auto pre_scan_base = (uintptr_t)swap_chain1 + base;

            // reached the end
            if (IsBadReadPtr((void*)pre_scan_base, sizeof(void*))) {
                break;
            }

            const auto scan_base = *(uintptr_t*)pre_scan_base;

            if (scan_base == 0 || IsBadReadPtr((void*)scan_base, sizeof(void*))) {
                continue;
            }

            for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
                const auto pre_data = scan_base + i;

                if (IsBadReadPtr((void*)pre_data, sizeof(void*))) {
                    break;
                }

                auto data = *(ID3D12CommandQueue**)pre_data;

                if (data == command_queue) {
                    // If we hook Streamline's Swapchain, the menu fails to render correctly/flickers
                    // So we switch out the swapchain with the internal one owned by Streamline
                    // Side note: Even though we are scanning for Proton here,
                    // this doubles as an offset scanner for the real swapchain inside Streamline (or FSR3)
                    if (m_using_frame_generation_swapchain) {
                        target_swapchain = (IDXGISwapChain3*)scan_base;
                    }

                    if (!m_using_frame_generation_swapchain) {
                        m_using_proton_swapchain = true;
                    }

                    s_command_queue_offset = i;
                    s_proton_swapchain_offset = base;
                    should_break = true;

                    spdlog::info("Proton potentially detected");
                    spdlog::info("Found command queue offset: {:x}", i);
                    break;
                }
            }

            if (m_using_proton_swapchain || should_break) {
                break;
            }
        }
    }

    if (s_command_queue_offset == 0) {
        spdlog::error("Failed to find command queue offset");
        return false;
    }

    //utility::ThreadSuspender suspender{};

    try {
        s_swapchain_vtable = *(void***)target_swapchain;
        s_factory_vtable = *(void***)factory;

        log_discovery_snapshot(target_swapchain, s_swapchain_vtable, factory, s_factory_vtable, command_queue);

        hook_impl();
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize hooks: {}", e.what());
        m_hooked = false;
    }

    //suspender.resume();

    command_queue->Release();
    swap_chain1->Release();
    swap_chain->Release();
    device->Release();
    factory->Release();

    if (hwnd) {
        ::DestroyWindow(hwnd);
    }

    if (wc.lpszClassName != nullptr) {
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    }

    return m_hooked;
}

void D3D12Hook::hook_impl() {
    spdlog::info("Initializing hooks");

    hook_streamline();

    m_present_hook.reset();
    m_swapchain_hook.reset();

    m_is_phase_1 = true;

    auto& present_fn = s_swapchain_vtable[8]; // Present
    m_present_hook = std::make_unique<PointerHook>(&present_fn, &D3D12Hook::present);

    const auto original_present = m_present_hook->get_original<decltype(D3D12Hook::present)*>();

    spdlog::info("[D3D12][HookInstall] phase = phase1, slot = Present[8], target = 0x{:x}, target_owner = {}, destination = D3D12Hook::present",
        reinterpret_cast<uintptr_t>(original_present), describe_address(reinterpret_cast<void*>(original_present)));

    if (s_create_swapchain_hook == nullptr) {
        auto& create_swapchain_fn = s_factory_vtable[15]; // CreateSwapChainForHwnd
        s_create_swapchain_hook = std::make_unique<PointerHook>(&create_swapchain_fn, &D3D12Hook::create_swapchain);
    }

    m_hooked = true;
}

bool D3D12Hook::unhook() {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    // Invalidate before the early return so no XeFG transaction can retain a
    // hook object while hook-monitor recovery destroys or replaces it.
    if (g_d3d12_hook == this) {
        g_d3d12_hook = nullptr;
    }

    if (!m_hooked && m_xefg_bound_swapchain == nullptr) {
        return true;
    }

    if (m_hooked) {
        spdlog::info("Unhooking D3D12");
    }

    const auto* owned_swapchain = m_xefg_bound_swapchain.Get();
    const auto* owned_queue = m_xefg_bound_queue.Get();
    const auto* owned_device = m_xefg_bound_device.Get();

    m_present_hook.reset();
    m_swapchain_hook.reset();

    if (m_swap_chain == owned_swapchain) {
        m_swap_chain = nullptr;
    }
    if (m_command_queue == owned_queue) {
        m_command_queue = nullptr;
    }
    if (m_device == owned_device) {
        m_device = nullptr;
    }

    m_xefg_bound_swapchain.Reset();
    m_xefg_bound_queue.Reset();
    m_xefg_bound_device.Reset();
    m_xefg_binding_generation = 0;

    m_hooked = false;
    m_is_phase_1 = true;

    return true;
}

thread_local int32_t g_present_depth = 0;

HRESULT WINAPI D3D12Hook::present(IDXGISwapChain3* swap_chain, uint64_t sync_interval, uint64_t flags, void* r9) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d12 = g_d3d12_hook;

    // XeFG Present and Present1 share the direct-binding lifecycle. Keep the
    // established native phase-1/instance path below unchanged.
    if (d3d12 != nullptr && !d3d12->m_is_phase_1 && d3d12->m_swapchain_source == SwapchainSource::XeFGInternal && d3d12->m_swapchain_hook != nullptr) {
        using PresentFn = decltype(D3D12Hook::present)*;
        const auto present_fn = d3d12->m_swapchain_hook->get_method<PresentFn>(8);
        return present_common(swap_chain, "Present", reinterpret_cast<void*>(present_fn), [swap_chain, sync_interval, flags, r9, present_fn]() {
            return present_fn(swap_chain, sync_interval, flags, r9);
        }, false);
    }

    decltype(D3D12Hook::present)* present_fn{nullptr};

    if (d3d12->m_is_phase_1) {
        present_fn = d3d12->m_present_hook->get_original<decltype(D3D12Hook::present)*>();
    } else {
        present_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::present)*>(8);
    }

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    const auto present_call = d3d12->m_present_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;
    d3d12->m_last_present_entry_time = std::chrono::steady_clock::now();

    const auto xefg_loaded = D3D12Hook::is_xefg_module_loaded();
    const auto should_log_present = present_call <= 10
        || d3d12->m_last_logged_present_swapchain != swap_chain
        || d3d12->m_last_logged_present_target != reinterpret_cast<void*>(present_fn)
        || d3d12->m_last_logged_present_phase_1 != d3d12->m_is_phase_1
        || d3d12->m_last_logged_present_xefg != xefg_loaded;

    if (should_log_present) {
        void* present_vtable = nullptr;
        if (const auto snapshot = snapshot_swapchain(swap_chain)) {
            present_vtable = snapshot->vtable;
        }

        spdlog::info("[D3D12][PresentEntry] call = {}, phase = {}, swapchain = 0x{:x}, vtable = 0x{:x}, hwnd = 0x{:x}, tracked_swapchain = 0x{:x}, original_present = 0x{:x}, original_owner = {}, thread_id = {}, xefg_module_loaded = {}",
            present_call,
            d3d12->m_is_phase_1 ? "phase1" : "instance",
            reinterpret_cast<uintptr_t>(swap_chain),
            reinterpret_cast<uintptr_t>(present_vtable),
            reinterpret_cast<uintptr_t>(swapchain_wnd),
            reinterpret_cast<uintptr_t>(d3d12->m_swap_chain),
            reinterpret_cast<uintptr_t>(present_fn),
            describe_address(reinterpret_cast<void*>(present_fn)),
            GetCurrentThreadId(),
            xefg_loaded);

        d3d12->m_last_logged_present_swapchain = swap_chain;
        d3d12->m_last_logged_present_target = reinterpret_cast<void*>(present_fn);
        d3d12->m_last_logged_present_phase_1 = d3d12->m_is_phase_1;
        d3d12->m_last_logged_present_xefg = xefg_loaded;
    }

    if (d3d12->m_is_phase_1 && WindowFilter::get().is_filtered(swapchain_wnd)) {
        //present_fn = d3d12->m_present_hook->get_original<decltype(D3D12Hook::present)*>();
        return present_fn(swap_chain, sync_interval, flags, r9);
    }

    if (!d3d12->m_is_phase_1 && swap_chain != d3d12->m_swapchain_hook->get_instance()) {
        return present_fn(swap_chain, sync_interval, flags, r9);
    }

    if (d3d12->m_is_phase_1) {
        // Remove the present hook, we will just rely on the vtable hook below
        // because we don't want to cause any conflicts with other hooks
        // vtable hooks are the least intrusive
        // And doing a global pointer replacement seems to have
        // conflicts with Streamline's hooks, causing unexplainable crashes
        d3d12->m_present_hook.reset();

        // vtable hook the swapchain instead of global hooking
        // this seems safer for whatever reason
        // if we globally hook the vtable pointers, it causes all sorts of weird conflicts with other hooks
        // dont hook present though via this hook so other hooks dont get confused
        d3d12->m_swapchain_hook = std::make_unique<VtableHook>(swap_chain);
        //d3d12->m_swapchain_hook->hook_method(2, (uintptr_t)&D3D12Hook::release);
        d3d12->m_swapchain_hook->hook_method(8, (uintptr_t)&D3D12Hook::present);
        d3d12->m_swapchain_hook->hook_method(22, (uintptr_t)&D3D12Hook::present1);
        d3d12->m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
        d3d12->m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);

        void* instance_vtable = nullptr;
        if (const auto snapshot = snapshot_swapchain(swap_chain)) {
            instance_vtable = snapshot->vtable;
            const auto instance_present_original = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::present)*>(8);
            spdlog::info("[D3D12][HookInstall] phase = instance, swapchain = 0x{:x}, vtable = 0x{:x}, Present[8].original = 0x{:x}, Present[8].owner = {}, ResizeBuffers[13] = 0x{:x}, ResizeTarget[14] = 0x{:x}",
                reinterpret_cast<uintptr_t>(swap_chain),
                reinterpret_cast<uintptr_t>(snapshot->vtable),
                reinterpret_cast<uintptr_t>(instance_present_original),
                describe_address(reinterpret_cast<void*>(instance_present_original)),
                reinterpret_cast<uintptr_t>(snapshot->resize_buffers),
                reinterpret_cast<uintptr_t>(snapshot->resize_target));
        }

        d3d12->m_is_phase_1 = false;

        spdlog::info("[D3D12][PhaseTransition] phase1 -> instance, swapchain = 0x{:x}, vtable = 0x{:x}, xefg_module_loaded = {}",
            reinterpret_cast<uintptr_t>(swap_chain),
            reinterpret_cast<uintptr_t>(instance_vtable),
            D3D12Hook::is_xefg_module_loaded());

        present_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::present)*>(8);
    }

    d3d12->m_inside_present = true;
    d3d12->m_swap_chain = swap_chain;

    {
        Microsoft::WRL::ComPtr<ID3D12Device4> temp_device{};
        swap_chain->GetDevice(IID_PPV_ARGS(&temp_device));
        d3d12->m_device = temp_device.Get();
    }

    if (d3d12->m_swapchain_source != SwapchainSource::XeFGInternal) {
        if (d3d12->m_using_proton_swapchain) {
            const auto real_swapchain = *(uintptr_t*)((uintptr_t)swap_chain + d3d12->s_proton_swapchain_offset);
            d3d12->m_command_queue = *(ID3D12CommandQueue**)(real_swapchain + d3d12->s_command_queue_offset);
        } else {
            d3d12->m_command_queue = *(ID3D12CommandQueue**)((uintptr_t)swap_chain + d3d12->s_command_queue_offset);
        }
    }

    if (d3d12->m_swapchain_0 == nullptr) {
        d3d12->m_swapchain_0 = swap_chain;
    } else if (d3d12->m_swapchain_1 == nullptr && swap_chain != d3d12->m_swapchain_0) {
        d3d12->m_swapchain_1 = swap_chain;
    }
    
    // Restore the original bytes
    // if an infinite loop occurs, this will prevent the game from crashing
    // while keeping our hook intact
    if (g_present_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{present_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{present_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(present_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Present fixed");
        }

        if ((uintptr_t)present_fn != (uintptr_t)D3D12Hook::present && g_present_depth == 1) {
            spdlog::info("Attempting to call real present function");

            ++g_present_depth;
            const auto result = present_fn(swap_chain, sync_interval, flags, r9);
            --g_present_depth;

            if (result != S_OK) {
                spdlog::error("Present failed: {:x}", result);
            }

            return result;
        }

        spdlog::info("Just returning S_OK");
        return S_OK;
    }

    if (d3d12->m_on_present) {
        d3d12->m_on_present(*d3d12);
    }

    ++g_present_depth;

    auto result = S_OK;
    
    if (!d3d12->m_ignore_next_present) {
        result = present_fn(swap_chain, sync_interval, flags, r9);

        if (result != S_OK) {
            spdlog::error("Present failed: {:x}", result);
        }
    } else {
        d3d12->m_ignore_next_present = false;
    }

    --g_present_depth;

    if (d3d12->m_on_post_present) {
        d3d12->m_on_post_present(*d3d12);
    }

    d3d12->m_inside_present = false;
    
    return result;
}

HRESULT D3D12Hook::present_common(IDXGISwapChain3* swap_chain, const char* kind, void* original_present, std::function<HRESULT()> original_call, bool allow_phase_transition) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};
    auto d3d12 = g_d3d12_hook;
    if (d3d12 == nullptr || swap_chain == nullptr || !original_call) {
        return E_FAIL;
    }

    if (allow_phase_transition && d3d12->m_is_phase_1) {
        return E_FAIL;
    }

    if (!d3d12->m_is_phase_1 && (d3d12->m_swapchain_hook == nullptr || swap_chain != d3d12->m_swapchain_hook->get_instance())) {
        return original_call();
    }

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);
    const auto present_call = d3d12->m_present_entry_count.fetch_add(1, std::memory_order_relaxed) + 1;
    d3d12->m_last_present_entry_time = std::chrono::steady_clock::now();

    const auto should_log_present = present_call <= 10
        || d3d12->m_last_logged_present_swapchain != swap_chain
        || d3d12->m_last_logged_present_target != original_present
        || d3d12->m_last_logged_present_phase_1 != d3d12->m_is_phase_1;

    if (should_log_present) {
        void* present_vtable = nullptr;
        if (const auto snapshot = snapshot_swapchain(swap_chain)) {
            present_vtable = snapshot->vtable;
        }

        spdlog::info("[D3D12][PresentEntry] call = {}, kind = {}, source = {}, phase = instance, swapchain = 0x{:x}, vtable = 0x{:x}, hwnd = 0x{:x}, tracked_swapchain = 0x{:x}, original_present = 0x{:x}, original_owner = {}, thread_id = {}",
            present_call,
            kind,
            d3d12->m_swapchain_source == SwapchainSource::XeFGInternal ? "xefg_internal" : "native",
            reinterpret_cast<uintptr_t>(swap_chain),
            reinterpret_cast<uintptr_t>(present_vtable),
            reinterpret_cast<uintptr_t>(swapchain_wnd),
            reinterpret_cast<uintptr_t>(d3d12->m_swap_chain),
            reinterpret_cast<uintptr_t>(original_present),
            describe_address(original_present),
            GetCurrentThreadId());

        d3d12->m_last_logged_present_swapchain = swap_chain;
        d3d12->m_last_logged_present_target = original_present;
        d3d12->m_last_logged_present_phase_1 = d3d12->m_is_phase_1;
    }

    d3d12->m_inside_present = true;
    d3d12->m_swap_chain = swap_chain;
    Microsoft::WRL::ComPtr<ID3D12Device4> temp_device{};
    if (SUCCEEDED(swap_chain->GetDevice(IID_PPV_ARGS(&temp_device)))) {
        d3d12->m_device = temp_device.Get();
    }

    if (d3d12->m_swapchain_source != SwapchainSource::XeFGInternal) {
        if (d3d12->m_using_proton_swapchain) {
            const auto real_swapchain = *(uintptr_t*)((uintptr_t)swap_chain + d3d12->s_proton_swapchain_offset);
            d3d12->m_command_queue = *(ID3D12CommandQueue**)(real_swapchain + d3d12->s_command_queue_offset);
        } else {
            d3d12->m_command_queue = *(ID3D12CommandQueue**)((uintptr_t)swap_chain + d3d12->s_command_queue_offset);
        }
    }

    if (g_present_depth > 0) {
        ++g_present_depth;
        const auto result = original_call();
        --g_present_depth;
        d3d12->m_inside_present = false;
        return result;
    }

    const auto suppress_render_callbacks = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
        && d3d12->m_xefg_p21_observe_only;
    const auto post_resize_ordinal = d3d12->m_xefg_post_resize_present_budget > 0
        ? d3d12->m_xefg_post_resize_present_ordinal + 1
        : 0;
    if (d3d12->m_swapchain_source == SwapchainSource::XeFGInternal && post_resize_ordinal > 0) {
        d3d12->log_xefg_post_resize_present(swap_chain, kind, original_present);
    }
    const auto log_render_boundary = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
        && !suppress_render_callbacks
        && d3d12->m_on_present
        && !d3d12->m_xefg_p21_render_boundary_logged;

    if (log_render_boundary) {
        spdlog::info("[XeFG][P2.1Probe] render_callback = enter, present_call = {}", present_call);
    }

    if (!suppress_render_callbacks && d3d12->m_on_present) {
        if (post_resize_ordinal == 1 && g_framework != nullptr) {
            g_framework->log_d3d12_resize_snapshot("present_pre_render_callback", d3d12->m_xefg_resize_event_id);
        }
        d3d12->m_on_present(*d3d12);

        if (post_resize_ordinal == 1 && g_framework != nullptr) {
            g_framework->log_d3d12_resize_snapshot("present_post_render_callback", d3d12->m_xefg_resize_event_id);
        }

        if (log_render_boundary) {
            spdlog::info("[XeFG][P2.1Probe] render_callback = returned, present_call = {}", present_call);
            d3d12->m_xefg_p21_render_boundary_logged = true;
        }
    }

    ++g_present_depth;
    HRESULT result = S_OK;
    if (!d3d12->m_ignore_next_present) {
        result = original_call();
        if (result != S_OK) {
            spdlog::error("{} failed: {:x}", kind, result);
        }
    } else {
        d3d12->m_ignore_next_present = false;
    }
    --g_present_depth;

    if (d3d12->m_swapchain_source == SwapchainSource::XeFGInternal && result == DXGI_ERROR_DEVICE_REMOVED) {
        const auto device_removed_reason = d3d12->m_device != nullptr ? d3d12->m_device->GetDeviceRemovedReason() : E_FAIL;
        spdlog::error("[XeFG][P2.1Probe] present_result = 0x{:08x}, device_removed_reason = 0x{:08x}",
            static_cast<uint32_t>(result), static_cast<uint32_t>(device_removed_reason));
    }

    if (suppress_render_callbacks) {
        // present_common already holds hook_monitor_mutex. Keep the monitor
        // alive without running renderer, GPU commit, or mod callbacks.
        g_framework->note_present_activity();
    } else if (d3d12->m_on_post_present) {
        d3d12->m_on_post_present(*d3d12);
    }

    d3d12->m_inside_present = false;
    return result;
}

uint64_t D3D12Hook::begin_xefg_resize_event(XefgResizeEventKind kind) {
    m_xefg_resize_event_id++;
    m_xefg_last_resize_kind = kind;
    m_xefg_last_resize_event_time = std::chrono::steady_clock::now();
    m_xefg_post_resize_present_budget = 3;
    m_xefg_post_resize_present_ordinal = 0;
    return m_xefg_resize_event_id;
}

const char* D3D12Hook::get_xefg_last_resize_kind() const {
    return resize_kind_name(m_xefg_last_resize_kind);
}

void D3D12Hook::log_xefg_resize_event(uint64_t event_id, XefgResizeEventKind kind, const char* stage,
    IDXGISwapChain3* swap_chain, void* original_fn, HRESULT result, bool has_result) const {
    Microsoft::WRL::ComPtr<IUnknown> identity{};
    if (swap_chain != nullptr) {
        swap_chain->QueryInterface(IID_PPV_ARGS(&identity));
    }

    const auto hook_instance = m_swapchain_hook != nullptr ? m_swapchain_hook->get_instance().ptr() : nullptr;
    const auto owner = describe_address(original_fn);
    if (has_result) {
        spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = {}, stage = {}, thread_id = {}, swapchain = 0x{:x}, swapchain_identity = 0x{:x}, tracked_swapchain = 0x{:x}, hook_instance = 0x{:x}, owned_swapchain = 0x{:x}, binding_generation = {}, command_queue = 0x{:x}, device = 0x{:x}, observe_only = {}, original_fn = 0x{:x}, original_owner = {}, result = 0x{:08x}",
            event_id, resize_kind_name(kind), stage, GetCurrentThreadId(),
            reinterpret_cast<uintptr_t>(swap_chain), reinterpret_cast<uintptr_t>(identity.Get()),
        reinterpret_cast<uintptr_t>(m_swap_chain), reinterpret_cast<uintptr_t>(hook_instance),
            reinterpret_cast<uintptr_t>(m_xefg_bound_swapchain.Get()), m_xefg_binding_generation,
            reinterpret_cast<uintptr_t>(m_command_queue), reinterpret_cast<uintptr_t>(m_device),
            m_xefg_p21_observe_only, reinterpret_cast<uintptr_t>(original_fn), owner,
            static_cast<uint32_t>(result));
    } else {
        spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = {}, stage = {}, thread_id = {}, swapchain = 0x{:x}, swapchain_identity = 0x{:x}, tracked_swapchain = 0x{:x}, hook_instance = 0x{:x}, owned_swapchain = 0x{:x}, binding_generation = {}, command_queue = 0x{:x}, device = 0x{:x}, observe_only = {}, original_fn = 0x{:x}, original_owner = {}",
            event_id, resize_kind_name(kind), stage, GetCurrentThreadId(),
            reinterpret_cast<uintptr_t>(swap_chain), reinterpret_cast<uintptr_t>(identity.Get()),
        reinterpret_cast<uintptr_t>(m_swap_chain), reinterpret_cast<uintptr_t>(hook_instance),
            reinterpret_cast<uintptr_t>(m_xefg_bound_swapchain.Get()), m_xefg_binding_generation,
            reinterpret_cast<uintptr_t>(m_command_queue), reinterpret_cast<uintptr_t>(m_device),
            m_xefg_p21_observe_only, reinterpret_cast<uintptr_t>(original_fn), owner);
    }
}

void D3D12Hook::log_xefg_post_resize_present(IDXGISwapChain3* swap_chain, const char* kind, void* original_fn) {
    if (m_xefg_post_resize_present_budget == 0) {
        return;
    }

    --m_xefg_post_resize_present_budget;
    const auto ordinal = ++m_xefg_post_resize_present_ordinal;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_xefg_last_resize_event_time).count();
    spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = {}, stage = present_after_resize, present_ordinal = {}, elapsed_ms_since_resize = {}, thread_id = {}, swapchain = 0x{:x}, tracked_swapchain = 0x{:x}, hook_instance = 0x{:x}, owned_swapchain = 0x{:x}, binding_generation = {}, command_queue = 0x{:x}, device = 0x{:x}, original_fn = 0x{:x}, original_owner = {}",
        m_xefg_resize_event_id,
        kind,
        ordinal,
        elapsed,
        GetCurrentThreadId(),
        reinterpret_cast<uintptr_t>(swap_chain),
        reinterpret_cast<uintptr_t>(m_swap_chain),
        reinterpret_cast<uintptr_t>(m_swapchain_hook != nullptr ? m_swapchain_hook->get_instance().ptr() : nullptr),
        reinterpret_cast<uintptr_t>(m_xefg_bound_swapchain.Get()),
        m_xefg_binding_generation,
        reinterpret_cast<uintptr_t>(m_command_queue),
        reinterpret_cast<uintptr_t>(m_device),
        reinterpret_cast<uintptr_t>(original_fn),
        describe_address(original_fn));
}

HRESULT WINAPI D3D12Hook::present1(IDXGISwapChain1* swap_chain, UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    // Hook-monitor recovery and swapchain recreation reset or destroy the active
    // D3D12Hook under this mutex. Keep it while reading both the hook object and
    // its vtable hook, then let present_common re-enter it recursively.
    std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};

    auto d3d12 = g_d3d12_hook;
    if (d3d12 == nullptr || d3d12->m_swapchain_hook == nullptr || swap_chain == nullptr) {
        return E_FAIL;
    }

    using Present1Fn = decltype(D3D12Hook::present1)*;
    const auto original = d3d12->m_swapchain_hook->get_method<Present1Fn>(22);
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain3;
    if (FAILED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain3)))) {
        return original(swap_chain, sync_interval, flags, parameters);
    }

    return present_common(swap_chain3.Get(), "Present1", reinterpret_cast<void*>(original), [swap_chain, sync_interval, flags, parameters, original]() {
        return original(swap_chain, sync_interval, flags, parameters);
    }, false);
}

thread_local int32_t g_resize_buffers_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize buffers called");
    spdlog::info(" Parameters: buffer_count {} width {} height {} new_format {} swap_chain_flags {}", buffer_count, width, height, new_format, swap_chain_flags);

    // Walk the callstack and print out module names
    try {
        std::string callstack_str{};
        for (const auto& entry : std::stacktrace::current()) {
            //spdlog::info(" {}", entry.description());
            callstack_str += entry.description() + "\n";
        }

        spdlog::info("callstack: \n{}", callstack_str); // because this can be running on a different thread and get garbled in the middle of the log
    } catch (const std::exception& e) {
        spdlog::error("Failed to print callstack: {}", e.what());
    } catch(...) {
        spdlog::error("Failed to print callstack: unknown exception");
    }

    auto d3d12 = g_d3d12_hook;
    //auto& hook = d3d12->m_resize_buffers_hook;
    //auto resize_buffers_fn = hook->get_original<decltype(D3D12Hook::resize_buffers)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    /*if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }*/

    auto resize_buffers_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers)*>(13);
    const auto resize_buffers_original = reinterpret_cast<void*>(resize_buffers_fn);

    const auto is_xefg_internal = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
        && swap_chain == d3d12->m_swapchain_hook->get_instance();
    const auto is_top_level = g_resize_buffers_depth == 0;
    const auto event_id = is_xefg_internal && is_top_level
        ? d3d12->begin_xefg_resize_event(D3D12Hook::XefgResizeEventKind::ResizeBuffers)
        : 0;
    if (event_id != 0) {
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers, "enter",
            swap_chain, resize_buffers_original);
        spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = ResizeBuffers, buffer_count = {}, width = {}, height = {}, format = {}, flags = 0x{:x}",
            event_id, buffer_count, width, height, static_cast<uint32_t>(new_format), swap_chain_flags);
    }

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;

    if (g_resize_buffers_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_buffers_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_buffers_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_buffers_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize buffers fixed");
        }

        if ((uintptr_t)resize_buffers_fn != (uintptr_t)&D3D12Hook::resize_buffers && g_resize_buffers_depth == 1) {
            spdlog::info("Attempting to call the real resize buffers function");

            ++g_resize_buffers_depth;
            const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
            --g_resize_buffers_depth;

            if (result != S_OK) {
                spdlog::error("Resize buffers failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_buffers) {
        if (event_id != 0) {
            d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers, "pre_reset",
                swap_chain, resize_buffers_original);
            g_framework->log_d3d12_resize_snapshot("resize_buffers_pre_reset", event_id);
        }
        d3d12->m_on_resize_buffers(*d3d12);
        if (event_id != 0) {
            d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers, "post_reset",
                swap_chain, resize_buffers_original);
            g_framework->log_d3d12_resize_snapshot("resize_buffers_post_reset", event_id);
        }
    }

    ++g_resize_buffers_depth;

    const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    
    if (result != S_OK) {
        spdlog::error("Resize buffers failed: {:x}", result);
    }

    --g_resize_buffers_depth;

    if (event_id != 0) {
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers, "original_return",
            swap_chain, resize_buffers_original, result, true);
    }

    return result;
}

thread_local int32_t g_resize_buffers1_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers1(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags, const UINT* creation_node_mask, IUnknown* const* present_queues) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock lifecycle_lock{g_framework->get_hook_monitor_mutex()};

    auto* d3d12 = g_d3d12_hook;
    if (d3d12 == nullptr || d3d12->m_swapchain_hook == nullptr || swap_chain == nullptr) {
        return E_FAIL;
    }

    using ResizeBuffers1Fn = decltype(D3D12Hook::resize_buffers1)*;
    const auto original = d3d12->m_swapchain_hook->get_method<ResizeBuffers1Fn>(39);
    const auto resize_buffers1_original = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(original));
    const auto is_tracked_instance = swap_chain == d3d12->m_swapchain_hook->get_instance();
    const auto is_xefg_internal = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal;

    if (!is_tracked_instance || !is_xefg_internal) {
        return original(swap_chain, buffer_count, width, height, new_format, swap_chain_flags, creation_node_mask, present_queues);
    }

    if (g_resize_buffers1_depth > 0) {
        ++g_resize_buffers1_depth;
        const auto nested_result = original(swap_chain, buffer_count, width, height, new_format, swap_chain_flags, creation_node_mask, present_queues);
        --g_resize_buffers1_depth;
        return nested_result;
    }

    const auto event_id = d3d12->begin_xefg_resize_event(D3D12Hook::XefgResizeEventKind::ResizeBuffers1);
    d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers1, "enter",
        swap_chain, resize_buffers1_original);
    spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = ResizeBuffers1, buffer_count = {}, width = {}, height = {}, format = {}, flags = 0x{:x}, creation_node_mask = 0x{:x}, present_queues = 0x{:x}",
        event_id, buffer_count, width, height, static_cast<uint32_t>(new_format), swap_chain_flags,
        creation_node_mask != nullptr ? *creation_node_mask : 0,
        reinterpret_cast<uintptr_t>(present_queues));

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;

    const auto should_reset_renderer = !d3d12->m_xefg_p21_observe_only && static_cast<bool>(d3d12->m_on_resize_buffers);
    spdlog::info("[XeFG][ResizeBuffers1] stage = enter, swapchain = 0x{:x}, buffer_count = {}, width = {}, height = {}, format = {}, flags = 0x{:x}, creation_node_mask = 0x{:x}, present_queues = 0x{:x}, pre_reset = {}",
        reinterpret_cast<uintptr_t>(swap_chain),
        buffer_count,
        width,
        height,
        static_cast<uint32_t>(new_format),
        swap_chain_flags,
        reinterpret_cast<uintptr_t>(creation_node_mask),
        reinterpret_cast<uintptr_t>(present_queues),
        should_reset_renderer);

    if (should_reset_renderer) {
        spdlog::info("[XeFG][ResizeBuffers1] stage = pre_reset_begin");
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers1, "pre_reset",
            swap_chain, resize_buffers1_original);
        g_framework->log_d3d12_resize_snapshot("resize_buffers1_pre_reset", event_id);
        d3d12->m_on_resize_buffers(*d3d12);
        spdlog::info("[XeFG][ResizeBuffers1] stage = pre_reset_end");
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers1, "post_reset",
            swap_chain, reinterpret_cast<void*>(original));
        g_framework->log_d3d12_resize_snapshot("resize_buffers1_post_reset", event_id);
    }

    ++g_resize_buffers1_depth;
    const auto result = original(swap_chain, buffer_count, width, height, new_format, swap_chain_flags, creation_node_mask, present_queues);
    --g_resize_buffers1_depth;

    spdlog::info("[XeFG][ResizeBuffers1] stage = original_return, result = 0x{:08x}", static_cast<uint32_t>(result));
    d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeBuffers1, "original_return",
            swap_chain, resize_buffers1_original, result, true);
    return result;
}

thread_local int32_t g_resize_target_depth = 0;

HRESULT WINAPI D3D12Hook::resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters) {
    while (g_framework == nullptr) {
        std::this_thread::yield();
    }

    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize target called");
    spdlog::info(" Parameters: new_target_parameters {:x}", (uintptr_t)new_target_parameters);

    // Walk the callstack and print out module names
    try {
        std::string callstack_str{};
        for (const auto& entry : std::stacktrace::current()) {
            //spdlog::info(" {}", entry.description());
            callstack_str += entry.description() + "\n";
        }

        spdlog::info("callstack: \n{}", callstack_str); // because this can be running on a different thread and get garbled in the middle of the log
    } catch (const std::exception& e) {
        spdlog::error("Failed to print callstack: {}", e.what());
    } catch(...) {
        spdlog::error("Failed to print callstack: unknown exception");
    }

    auto d3d12 = g_d3d12_hook;
    //auto resize_target_fn = d3d12->m_resize_target_hook->get_original<decltype(D3D12Hook::resize_target)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    /*if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_target_fn(swap_chain, new_target_parameters);
    }*/

    auto resize_target_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_target)*>(14);
    const auto resize_target_original = reinterpret_cast<void*>(resize_target_fn);

    const auto is_xefg_internal = d3d12->m_swapchain_source == SwapchainSource::XeFGInternal
        && swap_chain == d3d12->m_swapchain_hook->get_instance();
    const auto is_top_level = g_resize_target_depth == 0;
    const auto event_id = is_xefg_internal && is_top_level
        ? d3d12->begin_xefg_resize_event(D3D12Hook::XefgResizeEventKind::ResizeTarget)
        : 0;

    if (new_target_parameters != nullptr) {
        d3d12->m_render_width = new_target_parameters->Width;
        d3d12->m_render_height = new_target_parameters->Height;
    }

    if (event_id != 0) {
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeTarget, "enter",
            swap_chain, resize_target_original);
        spdlog::info("[XeFG][ResizeLifecycle] event_id = {}, kind = ResizeTarget, dimensions = {}x{}, refresh = {}/{}, format = {}, scanline_ordering = {}, scaling = {}, target_parameters = 0x{:x}",
            event_id,
            new_target_parameters != nullptr ? new_target_parameters->Width : 0,
            new_target_parameters != nullptr ? new_target_parameters->Height : 0,
            new_target_parameters != nullptr ? new_target_parameters->RefreshRate.Numerator : 0,
            new_target_parameters != nullptr ? new_target_parameters->RefreshRate.Denominator : 0,
            new_target_parameters != nullptr ? static_cast<uint32_t>(new_target_parameters->Format) : 0,
            new_target_parameters != nullptr ? static_cast<uint32_t>(new_target_parameters->ScanlineOrdering) : 0,
            new_target_parameters != nullptr ? static_cast<uint32_t>(new_target_parameters->Scaling) : 0,
            reinterpret_cast<uintptr_t>(new_target_parameters));
    }

    // Restore the original code to the resize_buffers function.
    if (g_resize_target_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_target_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_target_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_target_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize target fixed");
        }

        if ((uintptr_t)resize_target_fn != (uintptr_t)&D3D12Hook::resize_target && g_resize_target_depth == 1) {
            spdlog::info("Attempting to call the real resize target function");

            ++g_resize_target_depth;
            const auto result = resize_target_fn(swap_chain, new_target_parameters);
            --g_resize_target_depth;

            if (result != S_OK) {
                spdlog::error("Resize target failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_target) {
        if (event_id != 0) {
            d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeTarget, "pre_reset",
                swap_chain, resize_target_original);
            g_framework->log_d3d12_resize_snapshot("resize_target_pre_reset", event_id);
        }
        d3d12->m_on_resize_target(*d3d12);
        if (event_id != 0) {
            d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeTarget, "post_reset",
                swap_chain, resize_target_original);
            g_framework->log_d3d12_resize_snapshot("resize_target_post_reset", event_id);
        }
    }

    ++g_resize_target_depth;

    const auto result = resize_target_fn(swap_chain, new_target_parameters);
    
    if (result != S_OK) {
        spdlog::error("Resize target failed: {:x}", result);
    }

    --g_resize_target_depth;

    if (event_id != 0) {
        d3d12->log_xefg_resize_event(event_id, D3D12Hook::XefgResizeEventKind::ResizeTarget, "original_return",
            swap_chain, resize_target_original, result, true);
    }

    return result;
}
