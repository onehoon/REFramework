#include "mods/re4_xess/RE4XeSSRuntime.hpp"

#include <spdlog/spdlog.h>

#include <system_error>
#include <utility>

namespace {

template <typename Function>
Function resolve_xess_export(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

std::string result_message(const char* operation, xess_result_t result) {
    return std::string{ operation } + " returned XeSS result " + std::to_string(static_cast<int32_t>(result));
}

}

RE4XeSSRuntime::~RE4XeSSRuntime() {
    shutdown();
}

bool RE4XeSSRuntime::initialize(ID3D12Device* device, const std::filesystem::path& reframework_directory) {
    shutdown();

    if (device == nullptr) {
        fail("The RE4 D3D12 device is unavailable");
        return false;
    }

    if (reframework_directory.empty()) {
        fail("Could not resolve the directory containing the REFramework module");
        return false;
    }

    m_candidates = {
        reframework_directory / L"libxess.dll",
        reframework_directory / L"OptiScaler" / L"libxess.dll",
    };

    std::error_code path_error;
    for (const auto& candidate : m_candidates) {
        path_error.clear();
        const auto is_file = std::filesystem::is_regular_file(candidate, path_error);
        if (path_error) {
            fail("Could not inspect runtime candidate: " + path_error.message());
            return false;
        }

        if (is_file) {
            m_selected_path = candidate;
            break;
        }
    }

    if (m_selected_path.empty()) {
        fail("libxess.dll was not found in either supported REFramework-relative location");
        return false;
    }

    m_module = LoadLibraryExW(
        m_selected_path.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (m_module == nullptr) {
        const auto error = GetLastError();
        fail("LoadLibraryExW failed with Win32 error " + std::to_string(error));
        return false;
    }

    m_state = State::ModuleReady;

    std::string missing_export;
    if (!resolve_required_exports(missing_export)) {
        fail("Required public XeSS export is missing: " + missing_export);
        return false;
    }

    xess_version_t version{};
    const auto version_result = m_functions.get_version(&version);
    if (version_result != XESS_RESULT_SUCCESS) {
        fail(result_message("xessGetVersion", version_result));
        return false;
    }
    m_runtime_version = version;

    const auto create_result = m_functions.d3d12_create_context(device, &m_context);
    if (create_result != XESS_RESULT_SUCCESS) {
        fail(result_message("xessD3D12CreateContext", create_result));
        return false;
    }
    if (m_context == nullptr) {
        fail("xessD3D12CreateContext succeeded but returned a null context");
        return false;
    }

    m_state = State::ContextReady;
    return true;
}

void RE4XeSSRuntime::shutdown() noexcept {
    cleanup();
    m_failure_reason.clear();
    m_candidates = {};
    m_selected_path.clear();
    m_runtime_version.reset();
    m_state = State::Unloaded;
}

bool RE4XeSSRuntime::resolve_required_exports(std::string& missing_export) {
#define RESOLVE_REQUIRED_XESS_EXPORT(member, symbol) \
    m_functions.member = resolve_xess_export<decltype(m_functions.member)>(m_module, #symbol); \
    if (m_functions.member == nullptr) { \
        missing_export = #symbol; \
        return false; \
    }

    RESOLVE_REQUIRED_XESS_EXPORT(get_version, xessGetVersion)
    RESOLVE_REQUIRED_XESS_EXPORT(get_optimal_input_resolution, xessGetOptimalInputResolution)
    RESOLVE_REQUIRED_XESS_EXPORT(destroy_context, xessDestroyContext)
    RESOLVE_REQUIRED_XESS_EXPORT(set_velocity_scale, xessSetVelocityScale)
    RESOLVE_REQUIRED_XESS_EXPORT(d3d12_create_context, xessD3D12CreateContext)
    RESOLVE_REQUIRED_XESS_EXPORT(d3d12_init, xessD3D12Init)
    RESOLVE_REQUIRED_XESS_EXPORT(d3d12_execute, xessD3D12Execute)

#undef RESOLVE_REQUIRED_XESS_EXPORT
    return true;
}

void RE4XeSSRuntime::cleanup() noexcept {
    if (m_context != nullptr) {
        const auto result = m_functions.destroy_context != nullptr
            ? m_functions.destroy_context(m_context)
            : XESS_RESULT_ERROR_INVALID_CONTEXT;

        if (result != XESS_RESULT_SUCCESS) {
            spdlog::error("[RE4XeSS][Runtime] xessDestroyContext returned XeSS result {}", static_cast<int32_t>(result));
        } else {
            spdlog::info("[RE4XeSS][Runtime] XeSS context destroyed");
        }

        m_context = nullptr;
    }

    if (m_module != nullptr) {
        if (FreeLibrary(m_module) == FALSE) {
            spdlog::error("[RE4XeSS][Runtime] FreeLibrary failed with Win32 error {}", GetLastError());
        }
        m_module = nullptr;
    }

    m_functions = {};
    m_state = State::Unloaded;
}

void RE4XeSSRuntime::fail(std::string reason) {
    cleanup();
    m_failure_reason = std::move(reason);
    m_state = State::Faulted;
}
