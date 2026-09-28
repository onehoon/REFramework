#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>

#include <Windows.h>

#include "RE4XeSSApi.hpp"

class RE4XeSSRuntime final {
public:
    struct InputResolutionQuery {
        xess_2d_t optimal{};
        xess_2d_t minimum{};
        xess_2d_t maximum{};
    };

    enum class State : uint8_t {
        Unloaded,
        ModuleReady,
        ContextReady,
        Faulted,
    };

    RE4XeSSRuntime() = default;
    ~RE4XeSSRuntime();

    RE4XeSSRuntime(const RE4XeSSRuntime&) = delete;
    RE4XeSSRuntime& operator=(const RE4XeSSRuntime&) = delete;
    RE4XeSSRuntime(RE4XeSSRuntime&&) = delete;
    RE4XeSSRuntime& operator=(RE4XeSSRuntime&&) = delete;

    bool initialize(ID3D12Device* device, const std::filesystem::path& reframework_directory);
    bool bind_owner_thread(std::string& error);
    struct InitSignature {
        xess_2d_t output_resolution{};
        xess_quality_settings_t quality{ XESS_QUALITY_SETTING_QUALITY };
        uint32_t init_flags{ XESS_INIT_FLAG_INVERTED_DEPTH };
        float velocity_scale_x{};
        float velocity_scale_y{};
    };

    struct ExecuteDiagnostics {
        uint64_t frame_id{};
        uint64_t control_generation{};
        uint64_t device_reset_generation{};
        uint64_t submission_sequence{};
        uint32_t bridge_slot{};
        uint32_t output_width{};
        uint32_t output_height{};
        ID3D12Resource* original_velocity{};
    };

    bool initialize_sr(const InitSignature& signature, std::string& error);
    bool execute(
        ID3D12GraphicsCommandList* command_list,
        const xess_d3d12_execute_params_t& params,
        const ExecuteDiagnostics& diagnostics,
        std::string& error);
    void shutdown() noexcept;
    void quarantine() noexcept;
    std::optional<InputResolutionQuery> query_optimal_input_resolution(
        xess_2d_t output_resolution,
        xess_quality_settings_t quality,
        std::string& error) const;

    State state() const noexcept {
        return m_state;
    }

    bool sr_initialized() const noexcept {
        return m_sr_initialized;
    }

    bool is_owner_thread() const noexcept;

    DWORD owner_thread_id() const noexcept {
        return m_owner_thread_id.load(std::memory_order_acquire);
    }

    const std::string& failure_reason() const noexcept {
        return m_failure_reason;
    }

    const std::array<std::filesystem::path, 2>& candidates() const noexcept {
        return m_candidates;
    }

    const std::filesystem::path& selected_path() const noexcept {
        return m_selected_path;
    }

    const std::optional<xess_version_t>& runtime_version() const noexcept {
        return m_runtime_version;
    }

private:
    struct Functions {
        decltype(&xessGetVersion) get_version{};
        decltype(&xessGetOptimalInputResolution) get_optimal_input_resolution{};
        decltype(&xessDestroyContext) destroy_context{};
        decltype(&xessSetVelocityScale) set_velocity_scale{};
        decltype(&xessD3D12CreateContext) d3d12_create_context{};
        decltype(&xessD3D12Init) d3d12_init{};
        decltype(&xessD3D12Execute) d3d12_execute{};
    };

    bool resolve_required_exports(std::string& missing_export);
    bool bind_or_check_owner_thread(std::string& error);
    bool check_owner_thread(std::string_view operation, std::string& error) const;
    void cleanup() noexcept;
    void fail(std::string reason);

    HMODULE m_module{};
    Functions m_functions{};
    xess_context_handle_t m_context{};
    State m_state{ State::Unloaded };
    std::string m_failure_reason{};
    std::array<std::filesystem::path, 2> m_candidates{};
    std::filesystem::path m_selected_path{};
    std::optional<xess_version_t> m_runtime_version{};
    std::atomic<DWORD> m_owner_thread_id{};
    uint64_t m_execute_detail_control_generation{};
    uint32_t m_execute_transition_detail_count{};
    bool m_sr_initialized{};
    bool m_quarantined{};
};
