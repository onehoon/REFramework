#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include <Windows.h>

#include "RE4XeSSApi.hpp"

class RE4XeSSRuntime final {
public:
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
    void shutdown() noexcept;

    State state() const noexcept {
        return m_state;
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
};
