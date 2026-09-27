#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

#include "RE4XeSSFrame.hpp"

class RE4XeSSRuntime;

class RE4XeSSD3D12 final {
public:
    static constexpr uint32_t SLOT_COUNT = 8;

    struct Signature {
        uint32_t render_width{};
        uint32_t render_height{};
        uint32_t display_width{};
        uint32_t display_height{};
        DXGI_FORMAT color_format{ DXGI_FORMAT_UNKNOWN };

        bool operator==(const Signature&) const = default;
    };

    enum class SubmitResult : uint8_t {
        Submitted,
        Busy,
        Faulted,
        NotReady,
    };

    enum class PollResult : uint8_t {
        Idle,
        InFlight,
        DeviceRemoved,
        Quarantined,
        Faulted,
    };

    RE4XeSSD3D12() = default;
    ~RE4XeSSD3D12();

    RE4XeSSD3D12(const RE4XeSSD3D12&) = delete;
    RE4XeSSD3D12& operator=(const RE4XeSSD3D12&) = delete;

    bool initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        const Signature& signature,
        std::string& error);

    SubmitResult submit(
        const RE4XeSSFrame& frame,
        RE4XeSSRuntime& runtime,
        std::string& error);

    PollResult poll();
    bool idle() const noexcept;
    bool ready() const noexcept;
    bool has_generation() const noexcept;
    bool matches(const Signature& signature, ID3D12Device* device, ID3D12CommandQueue* queue) const noexcept;
    bool quarantined() const noexcept;
    const std::string& failure_reason() const noexcept;
    uint64_t last_submitted_fence_value() const noexcept;

    // Normal shutdown is legal only after PollResult::Idle or confirmed device
    // removal. Otherwise this object permanently quarantines its COM objects.
    void shutdown() noexcept;
    void shutdown_after_device_removed() noexcept;
    void quarantine() noexcept;

private:
    struct CommandSlot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> velocity_descriptors;
        Microsoft::WRL::ComPtr<ID3D12Resource> color_pin;
        Microsoft::WRL::ComPtr<ID3D12Resource> depth_pin;
        Microsoft::WRL::ComPtr<ID3D12Resource> original_velocity_pin;
        uint64_t last_fence_value{};
    };

    bool create_common_objects(std::string& error);
    bool create_generation_resources(std::string& error);
    bool create_slot_objects(std::string& error);
    bool create_conversion_pipeline(std::string& error);
    bool validate_format_support(DXGI_FORMAT format, std::string& error) const;
    bool validate_frame(const RE4XeSSFrame& frame, std::string& error) const;
    void write_slot_descriptors(CommandSlot& slot, ID3D12Resource* velocity);
    bool record_and_submit(
        CommandSlot& slot,
        const RE4XeSSFrame& frame,
        RE4XeSSRuntime& runtime,
        uint32_t slot_index,
        std::string& error);
    void set_name(ID3D12Object* object, const wchar_t* name) const noexcept;
    void quarantine_objects() noexcept;

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_converted_velocity;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_detached_output;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_velocity_root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_velocity_pipeline;
    std::array<CommandSlot, SLOT_COUNT> m_slots{};
    Signature m_signature{};
    D3D12_RESOURCE_STATES m_converted_velocity_state{ D3D12_RESOURCE_STATE_COMMON };
    UINT m_descriptor_increment{};
    uint64_t m_next_fence_value{ 1 };
    uint64_t m_last_submitted_fence_value{};
    uint64_t m_submission_count{};
    uint32_t m_next_slot{};
    uint64_t m_quarantine_fence_value{};
    bool m_initialized{};
    bool m_quarantined{};
    bool m_probe_quarantine_completion{};
    bool m_quarantine_probe_attempted{};
    bool m_device_removed{};
    std::string m_failure_reason{};
};
