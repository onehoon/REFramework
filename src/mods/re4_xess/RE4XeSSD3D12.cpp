#include "mods/re4_xess/RE4XeSSD3D12.hpp"

#include <array>
#include <limits>
#include <string>
#include <string_view>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include "mods/REFrameworkConfig.hpp"
#include "mods/re4_xess/RE4XeSSRuntime.hpp"

namespace {

constexpr std::string_view VELOCITY_SHADER = R"(
Texture2D<float4> InputVelocity : register(t0);
RWTexture2D<float2> OutputVelocity : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint width;
    uint height;
    OutputVelocity.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) {
        return;
    }
    OutputVelocity[tid.xy] = InputVelocity.Load(int3(tid.xy, 0)).rg;
}
)";

D3D12_HEAP_PROPERTIES default_heap_properties() {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 0;
    properties.VisibleNodeMask = 0;
    return properties;
}

D3D12_RESOURCE_DESC texture_desc(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return description;
}

void transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after,
    D3D12_RESOURCE_BARRIER& barrier) {
    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
}

std::string hresult_message(const char* operation, HRESULT result) {
    return std::string{ operation } + " failed with HRESULT 0x" +
        [] (HRESULT value) {
            constexpr char digits[] = "0123456789ABCDEF";
            std::string out(8, '0');
            const auto bits = static_cast<uint32_t>(value);
            for (size_t i = 0; i < out.size(); ++i) {
                out[out.size() - i - 1] = digits[(bits >> (i * 4)) & 0xF];
            }
            return out;
        }(result);
}

}

RE4XeSSD3D12::~RE4XeSSD3D12() {
    shutdown();
}

bool RE4XeSSD3D12::initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    const Signature& signature,
    std::string& error) {
    error.clear();

    if (device == nullptr || queue == nullptr) {
        error = "The RE4 D3D12 device or command queue is unavailable";
        return false;
    }
    if (signature.render_width == 0 || signature.render_height == 0 ||
        signature.display_width == 0 || signature.display_height == 0) {
        error = "The RE4 XeSS bridge signature contains a zero extent";
        return false;
    }
    if (signature.color_format != DXGI_FORMAT_R11G11B10_FLOAT) {
        error = "The RE4 XeSS bridge requires R11G11B10_FLOAT Color; received format " +
            std::to_string(static_cast<uint32_t>(signature.color_format));
        return false;
    }

    const auto queue_description = queue->GetDesc();
    if (queue_description.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "The active RE4 D3D12 command queue is not DIRECT";
        return false;
    }

    if (matches(signature, device, queue)) {
        return true;
    }

    if (m_initialized || m_quarantined) {
        const auto poll_result = poll();
        if (poll_result != PollResult::Idle) {
            error = poll_result == PollResult::DeviceRemoved
                ? "The previous RE4 XeSS bridge generation was device-removed"
                : "The previous RE4 XeSS bridge generation has not drained safely";
            return false;
        }
        shutdown();
    }

    m_device = device;
    m_queue = queue;
    m_signature = signature;
    m_failure_reason.clear();
    m_quarantined = false;
    m_device_removed = false;
    m_converted_velocity_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_fence_value = 1;
    m_last_submitted_fence_value = 0;
    m_submission_count = 0;
    m_next_slot = 0;
    m_quarantine_fence_value = 0;
    m_probe_quarantine_completion = false;
    m_quarantine_probe_attempted = false;

    if (!create_common_objects(error) || !create_generation_resources(error) ||
        !create_slot_objects(error) || !create_conversion_pipeline(error)) {
        m_failure_reason = error;
        shutdown();
        return false;
    }

    m_initialized = true;
    spdlog::info(
        "[RE4XeSS][Init] bridge device=0x{:x} queue=0x{:x} queueType={} render={}x{} display={}x{} convertedMV={} output={} ring={}",
        reinterpret_cast<uintptr_t>(device),
        reinterpret_cast<uintptr_t>(queue),
        static_cast<uint32_t>(queue_description.Type),
        signature.render_width,
        signature.render_height,
        signature.display_width,
        signature.display_height,
        static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<uint32_t>(signature.color_format),
        SLOT_COUNT);
    return true;
}

RE4XeSSD3D12::SubmitResult RE4XeSSD3D12::submit(
    const RE4XeSSFrame& frame,
    RE4XeSSRuntime& runtime,
    const OutputBinding& output,
    uint64_t control_generation,
    uint64_t device_reset_generation,
    std::string& error) {
    error.clear();
    if (!m_initialized || m_quarantined || m_device_removed) {
        error = m_failure_reason.empty() ? "The RE4 XeSS D3D12 bridge is not ready" : m_failure_reason;
        return SubmitResult::NotReady;
    }
    if (!validate_frame(frame, error)) {
        m_failure_reason = error;
        return SubmitResult::Faulted;
    }
    if (!validate_output(output, error)) {
        m_failure_reason = error;
        return SubmitResult::Faulted;
    }

    if (m_quarantined || m_device_removed) {
        error = m_failure_reason.empty() ? "The RE4 XeSS bridge cannot submit from a terminal generation" : m_failure_reason;
        return SubmitResult::Faulted;
    }

    uint32_t selected_slot = SLOT_COUNT;
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        const auto index = (m_next_slot + i) % SLOT_COUNT;
        auto& slot = m_slots[index];
        if (slot.last_fence_value == 0) {
            selected_slot = index;
            break;
        }
    }

    if (selected_slot == SLOT_COUNT) {
        return SubmitResult::Busy;
    }

    auto& slot = m_slots[selected_slot];
    slot.color_pin = frame.color;
    slot.depth_pin = frame.depth;
    slot.original_velocity_pin = frame.velocity;
    slot.output_pin = output.resource;
    write_slot_descriptors(slot, frame.velocity);

    if (!record_and_submit(
            slot,
            frame,
            runtime,
            output,
            selected_slot,
            control_generation,
            device_reset_generation,
            error)) {
        if (m_quarantined) {
            return SubmitResult::Faulted;
        }
        slot.color_pin.Reset();
        slot.depth_pin.Reset();
        slot.original_velocity_pin.Reset();
        slot.output_pin.Reset();
        m_failure_reason = error;
        return SubmitResult::Faulted;
    }

    m_next_slot = (selected_slot + 1) % SLOT_COUNT;
    return SubmitResult::Submitted;
}

RE4XeSSD3D12::PollResult RE4XeSSD3D12::poll() {
    if (m_device_removed) {
        return PollResult::DeviceRemoved;
    }
    if (m_fence == nullptr) {
        return m_quarantined ? PollResult::Quarantined : PollResult::Idle;
    }

    const auto completed = m_fence->GetCompletedValue();
    if (completed == std::numeric_limits<uint64_t>::max()) {
        m_device_removed = true;
        const auto reason = m_device != nullptr ? m_device->GetDeviceRemovedReason() : E_FAIL;
        m_failure_reason = hresult_message("D3D12 device removed", reason);
        spdlog::error("[RE4XeSS][Failure] {}", m_failure_reason);
        return PollResult::DeviceRemoved;
    }
    if (m_quarantined) {
        if (!m_probe_quarantine_completion) {
            return PollResult::Quarantined;
        }
        if (m_quarantine_fence_value != 0 && completed >= m_quarantine_fence_value) {
            for (auto& slot : m_slots) {
                slot.color_pin.Reset();
                slot.depth_pin.Reset();
                slot.original_velocity_pin.Reset();
                slot.output_pin.Reset();
                slot.last_fence_value = 0;
            }
            m_quarantined = false;
            m_probe_quarantine_completion = false;
            m_quarantine_fence_value = 0;
            m_quarantine_probe_attempted = false;
            spdlog::warn("[RE4XeSS][Failure] quarantined generation reached a later queue-fence completion; safe teardown is now permitted");
            return PollResult::Idle;
        }
        if (!m_quarantine_probe_attempted) {
            m_quarantine_probe_attempted = true;
            if (m_next_fence_value != 0 && m_next_fence_value != std::numeric_limits<uint64_t>::max()) {
                const auto marker_value = m_next_fence_value++;
                const auto signal_result = m_queue->Signal(m_fence.Get(), marker_value);
                if (SUCCEEDED(signal_result)) {
                    m_quarantine_fence_value = marker_value;
                    return PollResult::Quarantined;
                }

                const auto removed_reason = m_device->GetDeviceRemovedReason();
                if (FAILED(removed_reason)) {
                    m_device_removed = true;
                    m_failure_reason = hresult_message("Device removed after quarantine fence signal", removed_reason);
                    spdlog::error("[RE4XeSS][Failure] {}", m_failure_reason);
                    return PollResult::DeviceRemoved;
                }

                m_failure_reason += "; terminal fence probe failed with " +
                    hresult_message("CommandQueue::Signal", signal_result) +
                    " while GetDeviceRemovedReason returned S_OK; generation remains quarantined";
            } else {
                m_failure_reason += "; no non-sentinel fence value remains for a terminal completion probe; generation remains quarantined";
            }
            return PollResult::Quarantined;
        }
        return PollResult::Quarantined;
    }

    bool in_flight{};
    for (auto& slot : m_slots) {
        if (slot.last_fence_value != 0 && completed >= slot.last_fence_value) {
            slot.color_pin.Reset();
            slot.depth_pin.Reset();
            slot.original_velocity_pin.Reset();
            slot.output_pin.Reset();
            slot.last_fence_value = 0;
        } else if (slot.last_fence_value != 0) {
            in_flight = true;
        }
    }
    return in_flight ? PollResult::InFlight : PollResult::Idle;
}

bool RE4XeSSD3D12::idle() const noexcept {
    if (m_quarantined || m_device_removed) {
        return false;
    }
    for (const auto& slot : m_slots) {
        if (slot.last_fence_value != 0) {
            return false;
        }
    }
    return true;
}

bool RE4XeSSD3D12::ready() const noexcept {
    return m_initialized && !m_quarantined && !m_device_removed;
}

bool RE4XeSSD3D12::has_generation() const noexcept {
    return m_initialized || m_quarantined || m_device_removed || m_fence != nullptr;
}

bool RE4XeSSD3D12::matches(
    const Signature& signature,
    ID3D12Device* device,
    ID3D12CommandQueue* queue) const noexcept {
    return m_initialized && m_signature == signature && m_device.Get() == device && m_queue.Get() == queue;
}

bool RE4XeSSD3D12::quarantined() const noexcept {
    return m_quarantined;
}

const std::string& RE4XeSSD3D12::failure_reason() const noexcept {
    return m_failure_reason;
}

uint64_t RE4XeSSD3D12::last_submitted_fence_value() const noexcept {
    return m_last_submitted_fence_value;
}

void RE4XeSSD3D12::shutdown() noexcept {
    if (m_quarantined) {
        quarantine_objects();
        return;
    }
    if (!idle()) {
        m_quarantined = true;
        m_failure_reason = "Attempted bridge shutdown before submitted work was proven complete";
        quarantine_objects();
        return;
    }

    for (auto& slot : m_slots) {
        slot.color_pin.Reset();
        slot.depth_pin.Reset();
        slot.original_velocity_pin.Reset();
        slot.output_pin.Reset();
        slot.velocity_descriptors.Reset();
        slot.list.Reset();
        slot.allocator.Reset();
        slot.last_fence_value = 0;
    }
    m_velocity_pipeline.Reset();
    m_velocity_root_signature.Reset();
    m_converted_velocity.Reset();
    m_fence.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_initialized = false;
    m_probe_quarantine_completion = false;
    m_quarantine_probe_attempted = false;
    m_quarantine_fence_value = 0;
    m_signature = {};
    m_converted_velocity_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_slot = 0;
    m_last_submitted_fence_value = 0;
    m_submission_count = 0;
}

void RE4XeSSD3D12::shutdown_after_device_removed() noexcept {
    if (!m_device_removed) {
        shutdown();
        return;
    }

    for (auto& slot : m_slots) {
        slot.color_pin.Reset();
        slot.depth_pin.Reset();
        slot.original_velocity_pin.Reset();
        slot.output_pin.Reset();
        slot.velocity_descriptors.Reset();
        slot.list.Reset();
        slot.allocator.Reset();
        slot.last_fence_value = 0;
    }
    m_velocity_pipeline.Reset();
    m_velocity_root_signature.Reset();
    m_converted_velocity.Reset();
    m_fence.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_initialized = false;
    m_quarantined = false;
    m_device_removed = false;
    m_probe_quarantine_completion = false;
    m_quarantine_probe_attempted = false;
    m_quarantine_fence_value = 0;
    m_signature = {};
    m_converted_velocity_state = D3D12_RESOURCE_STATE_COMMON;
    m_next_slot = 0;
    m_last_submitted_fence_value = 0;
    m_submission_count = 0;
}

void RE4XeSSD3D12::quarantine() noexcept {
    if (m_quarantined) {
        quarantine_objects();
        return;
    }
    m_quarantined = true;
    m_probe_quarantine_completion = false;
    m_quarantine_probe_attempted = true;
    if (m_failure_reason.empty()) {
        m_failure_reason = "RE4 XeSS D3D12 bridge generation quarantined until process teardown";
    }
    quarantine_objects();
}

bool RE4XeSSD3D12::create_common_objects(std::string& error) {
    const auto queue_description = m_queue->GetDesc();
    const auto result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(result)) {
        error = hresult_message("CreateFence", result);
        return false;
    }
    set_name(m_fence.Get(), L"RE4XeSS Fence");
    (void)queue_description;
    return true;
}

bool RE4XeSSD3D12::create_generation_resources(std::string& error) {
    if (!validate_format_support(DXGI_FORMAT_R16G16_FLOAT, error) ||
        !validate_format_support(m_signature.color_format, error)) {
        return false;
    }

    const auto heap = default_heap_properties();
    const auto velocity_description = texture_desc(
        m_signature.render_width, m_signature.render_height, DXGI_FORMAT_R16G16_FLOAT);
    auto result = m_device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &velocity_description,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_converted_velocity));
    if (FAILED(result)) {
        error = hresult_message("CreateCommittedResource(converted velocity)", result);
        return false;
    }
    set_name(m_converted_velocity.Get(), L"RE4XeSS ConvertedVelocity");
    return true;
}

bool RE4XeSSD3D12::create_slot_objects(std::string& error) {
    m_descriptor_increment = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
        auto& slot = m_slots[i];
        auto result = m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(result)) {
            error = hresult_message("CreateCommandAllocator", result);
            return false;
        }
        result = m_device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            slot.allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&slot.list));
        if (FAILED(result)) {
            error = hresult_message("CreateCommandList", result);
            return false;
        }
        result = slot.list->Close();
        if (FAILED(result)) {
            error = hresult_message("Close(new command list)", result);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
        heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_description.NumDescriptors = 2;
        heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heap_description.NodeMask = 0;
        result = m_device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&slot.velocity_descriptors));
        if (FAILED(result)) {
            error = hresult_message("CreateDescriptorHeap(velocity)", result);
            return false;
        }

        auto name = std::wstring{ L"RE4XeSS CommandAllocator " } + std::to_wstring(i);
        set_name(slot.allocator.Get(), name.c_str());
        name = std::wstring{ L"RE4XeSS CommandList " } + std::to_wstring(i);
        set_name(slot.list.Get(), name.c_str());
        name = std::wstring{ L"RE4XeSS VelocityDescriptors " } + std::to_wstring(i);
        set_name(slot.velocity_descriptors.Get(), name.c_str());
    }
    return true;
}

bool RE4XeSSD3D12::create_conversion_pipeline(std::string& error) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_description{};
    root_description.NumParameters = 1;
    root_description.pParameters = &parameter;
    root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    auto result = D3D12SerializeRootSignature(
        &root_description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics);
    if (FAILED(result)) {
        error = hresult_message("D3D12SerializeRootSignature", result);
        if (diagnostics != nullptr) {
            error += ": ";
            error.append(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
        }
        return false;
    }

    result = m_device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_velocity_root_signature));
    if (FAILED(result)) {
        error = hresult_message("CreateRootSignature(velocity conversion)", result);
        return false;
    }
    set_name(m_velocity_root_signature.Get(), L"RE4XeSS VelocityConvertRootSignature");

    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    diagnostics.Reset();
    result = D3DCompile(
        VELOCITY_SHADER.data(),
        VELOCITY_SHADER.size(),
        "RE4XeSSVelocityConvert",
        nullptr,
        nullptr,
        "main",
        "cs_5_1",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &shader,
        &diagnostics);
    if (FAILED(result)) {
        error = hresult_message("D3DCompile(velocity conversion)", result);
        if (diagnostics != nullptr) {
            error += ": ";
            error.append(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
        }
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
    pipeline_description.pRootSignature = m_velocity_root_signature.Get();
    pipeline_description.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
    pipeline_description.NodeMask = 0;
    result = m_device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&m_velocity_pipeline));
    if (FAILED(result)) {
        error = hresult_message("CreateComputePipelineState(velocity conversion)", result);
        return false;
    }
    set_name(m_velocity_pipeline.Get(), L"RE4XeSS VelocityConvertPSO");
    return true;
}

bool RE4XeSSD3D12::validate_format_support(DXGI_FORMAT format, std::string& error) const {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = format;
    const auto result = m_device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(result)) {
        error = hresult_message("CheckFeatureSupport(FORMAT_SUPPORT)", result);
        return false;
    }

    constexpr auto required_support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    if ((support.Support1 & required_support1) != required_support1 ||
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        error = "Required typed UAV support is unavailable for DXGI format " +
            std::to_string(static_cast<uint32_t>(format)) + " (Support1=" +
            std::to_string(static_cast<uint32_t>(support.Support1)) + ", Support2=" +
            std::to_string(static_cast<uint32_t>(support.Support2)) + ")";
        return false;
    }
    return true;
}

bool RE4XeSSD3D12::validate_frame(const RE4XeSSFrame& frame, std::string& error) const {
    if (frame.color == nullptr || frame.depth == nullptr || frame.velocity == nullptr) {
        error = "The pre-Overlay frame is missing a required borrowed engine resource";
        return false;
    }
    if (frame.render_width != m_signature.render_width || frame.render_height != m_signature.render_height ||
        frame.display_width != m_signature.display_width || frame.display_height != m_signature.display_height) {
        error = "The pre-Overlay frame extent does not match the active bridge generation";
        return false;
    }

    const auto color = frame.color->GetDesc();
    const auto depth = frame.depth->GetDesc();
    const auto velocity = frame.velocity->GetDesc();
    const auto valid_extent = [&](const D3D12_RESOURCE_DESC& description, uint32_t width, uint32_t height) {
        return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            description.Width == width && description.Height == height &&
            description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1;
    };
    if (!valid_extent(color, frame.render_width, frame.render_height) || color.Format != m_signature.color_format) {
        error = "The RE4 Color resource format/extent does not match the XeSS bridge signature";
        return false;
    }
    if (!valid_extent(depth, frame.render_width, frame.render_height)) {
        error = "The RE4 Depth resource extent does not match the XeSS input extent";
        return false;
    }
    if (!valid_extent(velocity, frame.render_width, frame.render_height) ||
        velocity.Format != DXGI_FORMAT_R16G16B16A16_SNORM) {
        error = "The RE4 Velocity resource must be R16G16B16A16_SNORM at the XeSS input extent";
        return false;
    }
    return true;
}

bool RE4XeSSD3D12::validate_output(const OutputBinding& output, std::string& error) const {
    if (output.resource == nullptr) {
        error = "The RE4 XeSS output binding has no engine-visible resource";
        return false;
    }
    const auto description = output.resource->GetDesc();
    constexpr auto required_flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != m_signature.display_width ||
        description.Height != m_signature.display_height ||
        description.DepthOrArraySize != 1 || description.SampleDesc.Count != 1 ||
        description.Format != m_signature.color_format ||
        (description.Flags & required_flags) != required_flags) {
        error = "The supplied RE4 XeSS output must be display-resolution R11G11B10_FLOAT Texture2D with render-target and UAV flags";
        return false;
    }
    if (output.after_state != static_cast<D3D12_RESOURCE_STATES>(
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
        error = "The supplied RE4 XeSS output must return to the verified 0xC0 shader-readable state";
        return false;
    }
    if (output.before_state != D3D12_RESOURCE_STATE_COMMON &&
        output.before_state != output.after_state) {
        error = "The supplied RE4 XeSS output must begin in COMMON on first use or 0xC0 on later use";
        return false;
    }
    return true;
}

void RE4XeSSD3D12::write_slot_descriptors(CommandSlot& slot, ID3D12Resource* velocity) {
    const auto cpu = slot.velocity_descriptors->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(velocity, &srv, cpu);

    auto uav_cpu = cpu;
    uav_cpu.ptr += m_descriptor_increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(m_converted_velocity.Get(), nullptr, &uav, uav_cpu);

}

bool RE4XeSSD3D12::record_and_submit(
    CommandSlot& slot,
    const RE4XeSSFrame& frame,
    RE4XeSSRuntime& runtime,
    const OutputBinding& output,
    uint32_t slot_index,
    uint64_t control_generation,
    uint64_t device_reset_generation,
    std::string& error) {
    auto result = slot.allocator->Reset();
    if (FAILED(result)) {
        error = hresult_message("CommandAllocator::Reset", result);
        return false;
    }
    result = slot.list->Reset(slot.allocator.Get(), nullptr);
    if (FAILED(result)) {
        error = hresult_message("GraphicsCommandList::Reset", result);
        return false;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> start_barriers{};
    size_t start_barrier_count{};
    if (m_converted_velocity_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        transition_barrier(
            m_converted_velocity.Get(),
            m_converted_velocity_state,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            start_barriers[start_barrier_count++]);
    }
    transition_barrier(
        frame.velocity,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        start_barriers[start_barrier_count++]);
    slot.list->ResourceBarrier(static_cast<UINT>(start_barrier_count), start_barriers.data());

    D3D12_RESOURCE_BARRIER output_start{};
    transition_barrier(
        output.resource,
        output.before_state,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        output_start);
    slot.list->ResourceBarrier(1, &output_start);

    ID3D12DescriptorHeap* heaps[]{ slot.velocity_descriptors.Get() };
    slot.list->SetDescriptorHeaps(1, heaps);
    slot.list->SetComputeRootSignature(m_velocity_root_signature.Get());
    slot.list->SetPipelineState(m_velocity_pipeline.Get());
    slot.list->SetComputeRootDescriptorTable(0, slot.velocity_descriptors->GetGPUDescriptorHandleForHeapStart());
    slot.list->Dispatch((frame.render_width + 7) / 8, (frame.render_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER converted_barrier{};
    transition_barrier(
        m_converted_velocity.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        converted_barrier);
    slot.list->ResourceBarrier(1, &converted_barrier);

    xess_d3d12_execute_params_t params{};
    params.pColorTexture = frame.color;
    params.pVelocityTexture = m_converted_velocity.Get();
    params.pDepthTexture = frame.depth;
    params.pExposureScaleTexture = nullptr;
    params.pResponsivePixelMaskTexture = nullptr;
    params.pOutputTexture = output.resource;
    params.jitterOffsetX = frame.jitter_x_pixels;
    params.jitterOffsetY = frame.jitter_y_pixels;
    params.exposureScale = 1.0f;
    params.resetHistory = frame.reset_history ? 1u : 0u;
    params.inputWidth = frame.render_width;
    params.inputHeight = frame.render_height;
    params.inputColorBase = { 0, 0 };
    params.inputMotionVectorBase = { 0, 0 };
    params.inputDepthBase = { 0, 0 };
    params.inputResponsiveMaskBase = { 0, 0 };
    params.reserved0 = { 0, 0 };
    params.outputColorBase = { 0, 0 };
    params.pDescriptorHeap = nullptr;
    params.descriptorHeapOffset = 0;

    RE4XeSSRuntime::ExecuteDiagnostics execute_diagnostics{};
    execute_diagnostics.frame_id = frame.frame_id;
    execute_diagnostics.control_generation = control_generation;
    execute_diagnostics.device_reset_generation = device_reset_generation;
    execute_diagnostics.submission_sequence = m_submission_count + 1;
    execute_diagnostics.bridge_slot = slot_index;
    execute_diagnostics.output_width = frame.display_width;
    execute_diagnostics.output_height = frame.display_height;
    execute_diagnostics.original_velocity = frame.velocity;

    if (!runtime.execute(slot.list.Get(), params, execute_diagnostics, error)) {
        slot.list->Close();
        return false;
    }

    D3D12_RESOURCE_BARRIER restore_velocity{};
    transition_barrier(
        frame.velocity,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        restore_velocity);
    slot.list->ResourceBarrier(1, &restore_velocity);

    D3D12_RESOURCE_BARRIER output_finish{};
    transition_barrier(
        output.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        output.after_state,
        output_finish);
    slot.list->ResourceBarrier(1, &output_finish);

    result = slot.list->Close();
    if (FAILED(result)) {
        error = hresult_message("GraphicsCommandList::Close", result);
        return false;
    }

    if (m_next_fence_value == 0 || m_next_fence_value == std::numeric_limits<uint64_t>::max()) {
        error = "The RE4 XeSS fence value space is exhausted before queue submission";
        return false;
    }

    ID3D12CommandList* lists[]{ slot.list.Get() };
    m_queue->ExecuteCommandLists(1, lists);
    const auto fence_value = m_next_fence_value++;
    result = m_queue->Signal(m_fence.Get(), fence_value);
    if (FAILED(result)) {
        m_quarantined = true;
        m_probe_quarantine_completion = true;
        m_quarantine_probe_attempted = false;
        m_quarantine_fence_value = 0;
        const auto removed_reason = m_device->GetDeviceRemovedReason();
        if (FAILED(removed_reason)) {
            m_device_removed = true;
        }
        error = hresult_message("CommandQueue::Signal", result) + "; " +
            hresult_message("GetDeviceRemovedReason", removed_reason);
        m_failure_reason = error;
        spdlog::error("[RE4XeSS][Failure] post-submit signal failure; generation quarantined: {}", error);
        return false;
    }

    slot.last_fence_value = fence_value;
    m_last_submitted_fence_value = fence_value;
    m_converted_velocity_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ++m_submission_count;

    if (REFrameworkConfig::get()->is_debug_log_enabled() && m_submission_count <= 32) {
        spdlog::info(
            "[RE4XeSS][Execute] frame={} slot={} fence={} resetHistory={} jitter=({:.5f},{:.5f}) input={}x{} output={}x{} color=0x{:x} depth=0x{:x} originalMV=0x{:x} convertedMV=0x{:x} handoffOutput=0x{:x} beforeState=0x{:x} afterState=0x{:x} result=SUCCESS",
            static_cast<unsigned long long>(frame.frame_id),
            slot_index,
            static_cast<unsigned long long>(fence_value),
            frame.reset_history,
            frame.jitter_x_pixels,
            frame.jitter_y_pixels,
            frame.render_width,
            frame.render_height,
            frame.display_width,
            frame.display_height,
            reinterpret_cast<uintptr_t>(frame.color),
            reinterpret_cast<uintptr_t>(frame.depth),
            reinterpret_cast<uintptr_t>(frame.velocity),
            reinterpret_cast<uintptr_t>(m_converted_velocity.Get()),
            reinterpret_cast<uintptr_t>(output.resource),
            static_cast<uint32_t>(output.before_state),
            static_cast<uint32_t>(output.after_state));
    }
    return true;
}

void RE4XeSSD3D12::set_name(ID3D12Object* object, const wchar_t* name) const noexcept {
    if (object != nullptr && name != nullptr) {
        object->SetName(name);
    }
}

void RE4XeSSD3D12::quarantine_objects() noexcept {
    for (auto& slot : m_slots) {
        (void)slot.color_pin.Detach();
        (void)slot.depth_pin.Detach();
        (void)slot.original_velocity_pin.Detach();
        (void)slot.output_pin.Detach();
        (void)slot.velocity_descriptors.Detach();
        (void)slot.list.Detach();
        (void)slot.allocator.Detach();
        slot.last_fence_value = 0;
    }
    (void)m_velocity_pipeline.Detach();
    (void)m_velocity_root_signature.Detach();
    (void)m_converted_velocity.Detach();
    (void)m_fence.Detach();
    (void)m_queue.Detach();
    (void)m_device.Detach();
    m_initialized = false;
}
