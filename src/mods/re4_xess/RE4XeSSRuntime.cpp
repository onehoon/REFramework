#include "mods/re4_xess/RE4XeSSRuntime.hpp"

#include <atomic>
#include <cmath>
#include <exception>
#include <spdlog/spdlog.h>

#include "mods/REFrameworkConfig.hpp"

#include <system_error>
#include <utility>

namespace {

std::atomic_flag owner_thread_violation_logged = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> execute_api_log_count{};
std::atomic_flag execute_api_exception_logged = ATOMIC_FLAG_INIT;
std::atomic_flag execute_api_failure_logged = ATOMIC_FLAG_INIT;
constexpr uint32_t MAX_EXECUTE_API_LOGS = 128;

struct NativeExceptionObservation {
    DWORD code{};
    DWORD flags{};
    uintptr_t exception_address{};
    uintptr_t instruction_pointer{};
    uintptr_t rax{};
    uintptr_t rcx{};
    uintptr_t rdx{};
    uintptr_t r8{};
    uintptr_t r9{};
    uintptr_t r10{};
    uintptr_t r11{};
    uintptr_t rsp{};
    uintptr_t rbp{};
    ULONG parameter_count{};
    ULONG_PTR information0{};
    ULONG_PTR information1{};
    bool registers_valid{};
    bool valid{};
};

LONG capture_native_exception(EXCEPTION_POINTERS* exception_info, NativeExceptionObservation* observation) noexcept {
    if (exception_info != nullptr && exception_info->ExceptionRecord != nullptr && observation != nullptr) {
        const auto* record = exception_info->ExceptionRecord;
        observation->code = record->ExceptionCode;
        observation->flags = record->ExceptionFlags;
        observation->exception_address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
        observation->instruction_pointer = exception_info->ContextRecord != nullptr
            ? static_cast<uintptr_t>(exception_info->ContextRecord->Rip)
            : 0;
#if defined(_M_X64)
        if (exception_info->ContextRecord != nullptr) {
            const auto* context = exception_info->ContextRecord;
            observation->rax = static_cast<uintptr_t>(context->Rax);
            observation->rcx = static_cast<uintptr_t>(context->Rcx);
            observation->rdx = static_cast<uintptr_t>(context->Rdx);
            observation->r8 = static_cast<uintptr_t>(context->R8);
            observation->r9 = static_cast<uintptr_t>(context->R9);
            observation->r10 = static_cast<uintptr_t>(context->R10);
            observation->r11 = static_cast<uintptr_t>(context->R11);
            observation->rsp = static_cast<uintptr_t>(context->Rsp);
            observation->rbp = static_cast<uintptr_t>(context->Rbp);
            observation->registers_valid = true;
        }
#endif
        observation->parameter_count = record->NumberParameters;
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            observation->information0 = record->ExceptionInformation[0];
            observation->information1 = record->ExceptionInformation[1];
        }
        observation->valid = true;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename ExecuteFunction>
xess_result_t invoke_xess_execute(
    ExecuteFunction function,
    xess_context_handle_t context,
    ID3D12GraphicsCommandList* command_list,
    const xess_d3d12_execute_params_t* params,
    NativeExceptionObservation* observation) {
    xess_result_t result{};
#if defined(_WIN32) && defined(_MSC_VER)
    __try {
        result = function(context, command_list, params);
    } __except (capture_native_exception(GetExceptionInformation(), observation)) {
        // The filter always continues search; this handler must never turn an
        // external exception into a normal XeSS return.
        std::terminate();
    }
#else
    result = function(context, command_list, params);
#endif
    return result;
}

void log_native_exception(
    const NativeExceptionObservation& observation,
    uint64_t frame_id,
    uint64_t control_generation,
    uint64_t device_reset_generation,
    uint64_t submission_sequence,
    uint32_t bridge_slot,
    uint32_t output_width,
    uint32_t output_height,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* original_velocity,
    const xess_d3d12_execute_params_t& params) {
    if (!observation.valid) {
        return;
    }

    const auto faulting_address = observation.instruction_pointer != 0
        ? observation.instruction_pointer
        : observation.exception_address;
    HMODULE faulting_module{};
    std::string module_path{ "unresolved" };
    uintptr_t module_rva{};
    if (faulting_address != 0 && GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(faulting_address),
            &faulting_module)) {
        const auto module_base = reinterpret_cast<uintptr_t>(faulting_module);
        module_rva = faulting_address >= module_base ? faulting_address - module_base : 0;
        std::array<wchar_t, 32768> wide_path{};
        const auto path_length = GetModuleFileNameW(
            faulting_module,
            wide_path.data(),
            static_cast<DWORD>(wide_path.size()));
        if (path_length != 0) {
            const auto utf8_length = WideCharToMultiByte(
                CP_UTF8,
                0,
                wide_path.data(),
                static_cast<int>(path_length),
                nullptr,
                0,
                nullptr,
                nullptr);
            if (utf8_length > 0) {
                module_path.resize(static_cast<size_t>(utf8_length));
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    wide_path.data(),
                    static_cast<int>(path_length),
                    module_path.data(),
                    utf8_length,
                    nullptr,
                    nullptr);
            }
        }
    }

    const auto access_kind = observation.code == EXCEPTION_ACCESS_VIOLATION && observation.parameter_count >= 2
        ? observation.information0 == 0 ? "read" : observation.information0 == 1 ? "write" : observation.information0 == 8 ? "execute" : "unknown"
        : "n/a";
    const auto faulting_address_argument = observation.code == EXCEPTION_ACCESS_VIOLATION && observation.parameter_count >= 2
        ? static_cast<uintptr_t>(observation.information1)
        : 0;
    spdlog::error(
        "[RE4XeSS][Execute] native-exception frame={} workerThread={} controlGeneration={} resetGeneration={} slot={} submission={} commandList=0x{:x} color=0x{:x} depth=0x{:x} convertedMV=0x{:x} originalMV=0x{:x} output=0x{:x} input={}x{} outputExtent={}x{} resetHistory={} code=0x{:08x} ExceptionAddress=0x{:x} Rip=0x{:x} module={} moduleRVA=0x{:x} ExceptionFlags=0x{:x} NumberParameters={} accessKind={} targetAddress=0x{:x} GPRValid={} RAX=0x{:x} RCX=0x{:x} RDX=0x{:x} R8=0x{:x} R9=0x{:x} R10=0x{:x} R11=0x{:x} RSP=0x{:x} RBP=0x{:x}",
        static_cast<unsigned long long>(frame_id),
        GetCurrentThreadId(),
        static_cast<unsigned long long>(control_generation),
        static_cast<unsigned long long>(device_reset_generation),
        bridge_slot,
        static_cast<unsigned long long>(submission_sequence),
        reinterpret_cast<uintptr_t>(command_list),
        reinterpret_cast<uintptr_t>(params.pColorTexture),
        reinterpret_cast<uintptr_t>(params.pDepthTexture),
        reinterpret_cast<uintptr_t>(params.pVelocityTexture),
        reinterpret_cast<uintptr_t>(original_velocity),
        reinterpret_cast<uintptr_t>(params.pOutputTexture),
        params.inputWidth,
        params.inputHeight,
        output_width,
        output_height,
        params.resetHistory != 0,
        observation.code,
        observation.exception_address,
        observation.instruction_pointer,
        module_path,
        module_rva,
        observation.flags,
        observation.parameter_count,
        access_kind,
        faulting_address_argument,
        observation.registers_valid,
        observation.rax,
        observation.rcx,
        observation.rdx,
        observation.r8,
        observation.r9,
        observation.r10,
        observation.r11,
        observation.rsp,
        observation.rbp);
}

void log_owner_thread_violation_once(const std::string& error) {
    if (!owner_thread_violation_logged.test_and_set(std::memory_order_relaxed)) {
        spdlog::error("[RE4XeSS][Failure] {}", error);
    }
}

template <typename Function>
Function resolve_xess_export(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

std::string result_message(const char* operation, xess_result_t result) {
    return std::string{ operation } + " returned XeSS result " + std::to_string(static_cast<int32_t>(result));
}

std::string path_for_log(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

}

RE4XeSSRuntime::~RE4XeSSRuntime() {
    shutdown();
}

bool RE4XeSSRuntime::initialize(ID3D12Device* device, const std::filesystem::path& reframework_directory) {
    std::string owner_error;
    if (!bind_or_check_owner_thread(owner_error)) {
        m_failure_reason = std::move(owner_error);
        m_state = State::Faulted;
        return false;
    }

    if (m_quarantined) {
        m_failure_reason = "The XeSS runtime is quarantined and cannot be initialized again";
        m_state = State::Faulted;
        return false;
    }

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

    for (size_t index = 0; index < m_candidates.size(); ++index) {
        const auto& candidate = m_candidates[index];
        const auto attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                spdlog::info("[RE4XeSS][Runtime] candidate {} missing; continuing: {}",
                    index + 1, path_for_log(candidate));
                continue;
            }

            fail("Could not inspect runtime candidate '" + path_for_log(candidate) +
                "', Win32 error " + std::to_string(error));
            return false;
        }

        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            spdlog::info("[RE4XeSS][Runtime] candidate {} is a directory; continuing: {}",
                index + 1, path_for_log(candidate));
            continue;
        }

        m_selected_path = candidate;
        spdlog::info("[RE4XeSS][Runtime] candidate {} selected: {}",
            index + 1, path_for_log(candidate));
        break;
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
        spdlog::error("[RE4XeSS][Runtime] LoadLibraryExW failed for '{}' with Win32 error {}",
            path_for_log(m_selected_path), error);
        fail("LoadLibraryExW failed for '" + path_for_log(m_selected_path) +
            "' with Win32 error " + std::to_string(error));
        return false;
    }

    spdlog::info("[RE4XeSS][Runtime] LoadLibraryExW succeeded for exact path '{}'",
        path_for_log(m_selected_path));

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

std::optional<RE4XeSSRuntime::InputResolutionQuery> RE4XeSSRuntime::query_optimal_input_resolution(
    xess_2d_t output_resolution,
    xess_quality_settings_t quality,
    std::string& error) const {
    error.clear();

    if (!check_owner_thread("xessGetOptimalInputResolution", error)) {
        return std::nullopt;
    }

    if (m_state != State::ContextReady || m_context == nullptr || m_functions.get_optimal_input_resolution == nullptr) {
        error = "The XeSS runtime context is not ready for an input-resolution query";
        return std::nullopt;
    }

    if (output_resolution.x == 0 || output_resolution.y == 0) {
        error = "The display/output resolution is zero";
        return std::nullopt;
    }

    InputResolutionQuery query{};
    const auto result = m_functions.get_optimal_input_resolution(
        m_context,
        &output_resolution,
        quality,
        &query.optimal,
        &query.minimum,
        &query.maximum);
    if (result != XESS_RESULT_SUCCESS) {
        error = result_message("xessGetOptimalInputResolution", result);
        return std::nullopt;
    }

    if (query.optimal.x == 0 || query.optimal.y == 0) {
        error = "xessGetOptimalInputResolution returned a zero optimal input extent";
        return std::nullopt;
    }

    const bool all_range_dimensions_zero =
        query.minimum.x == 0 && query.minimum.y == 0 &&
        query.maximum.x == 0 && query.maximum.y == 0;
    const bool all_range_dimensions_nonzero =
        query.minimum.x != 0 && query.minimum.y != 0 &&
        query.maximum.x != 0 && query.maximum.y != 0;

    if (!all_range_dimensions_zero && !all_range_dimensions_nonzero) {
        error = "xessGetOptimalInputResolution returned mixed zero/nonzero min/max metadata";
        return std::nullopt;
    }

    if (all_range_dimensions_nonzero) {
        if (query.minimum.x > query.maximum.x || query.minimum.y > query.maximum.y ||
            query.optimal.x < query.minimum.x || query.optimal.x > query.maximum.x ||
            query.optimal.y < query.minimum.y || query.optimal.y > query.maximum.y) {
            error = "xessGetOptimalInputResolution returned an optimal extent outside its min/max range";
            return std::nullopt;
        }
    }

    return query;
}

bool RE4XeSSRuntime::bind_owner_thread(std::string& error) {
    return bind_or_check_owner_thread(error);
}

bool RE4XeSSRuntime::initialize_sr(const InitSignature& signature, std::string& error) {
    error.clear();

    if (!check_owner_thread("xessD3D12Init", error)) {
        return false;
    }
    if (m_state != State::ContextReady || m_context == nullptr ||
        m_functions.d3d12_init == nullptr || m_functions.set_velocity_scale == nullptr) {
        error = "The XeSS runtime context is not ready for D3D12 initialization";
        return false;
    }
    if (signature.output_resolution.x == 0 || signature.output_resolution.y == 0 ||
        !std::isfinite(signature.velocity_scale_x) || !std::isfinite(signature.velocity_scale_y)) {
        error = "The XeSS D3D12 initialization signature is invalid";
        return false;
    }

    xess_d3d12_init_params_t params{};
    params.outputResolution = signature.output_resolution;
    params.qualitySetting = signature.quality;
    params.initFlags = signature.init_flags;
    params.creationNodeMask = 0;
    params.visibleNodeMask = 0;
    params.pTempBufferHeap = nullptr;
    params.bufferHeapOffset = 0;
    params.pTempTextureHeap = nullptr;
    params.textureHeapOffset = 0;
    params.pPipelineLibrary = nullptr;

    m_sr_initialized = false;
    const auto init_result = m_functions.d3d12_init(m_context, &params);
    if (init_result != XESS_RESULT_SUCCESS) {
        error = result_message("xessD3D12Init", init_result);
        m_failure_reason = error;
        m_state = State::Faulted;
        return false;
    }

    const auto velocity_result = m_functions.set_velocity_scale(
        m_context, signature.velocity_scale_x, signature.velocity_scale_y);
    if (velocity_result != XESS_RESULT_SUCCESS) {
        error = result_message("xessSetVelocityScale", velocity_result);
        m_failure_reason = error;
        m_state = State::Faulted;
        return false;
    }

    m_sr_initialized = true;
    return true;
}

bool RE4XeSSRuntime::execute(
    ID3D12GraphicsCommandList* command_list,
    const xess_d3d12_execute_params_t& params,
    const ExecuteDiagnostics& diagnostics,
    std::string& error) {
    error.clear();

    if (!check_owner_thread("xessD3D12Execute", error)) {
        return false;
    }
    if (!m_sr_initialized || m_context == nullptr || m_functions.d3d12_execute == nullptr) {
        error = "The XeSS D3D12 producer is not initialized";
        return false;
    }
    if (command_list == nullptr) {
        error = "The XeSS D3D12 command list is null";
        return false;
    }

    const bool debug_log = REFrameworkConfig::get() != nullptr &&
        REFrameworkConfig::get()->is_debug_log_enabled();
    const auto log_index = debug_log
        ? execute_api_log_count.fetch_add(1, std::memory_order_relaxed)
        : MAX_EXECUTE_API_LOGS;
    const bool log_api_call = log_index < MAX_EXECUTE_API_LOGS;
    const auto log_context = [&](spdlog::level::level_enum level, std::string_view stage, std::string_view detail) {
        spdlog::log(level,
            "[RE4XeSS][Execute] {} frame={} workerThread={} controlGeneration={} resetGeneration={} slot={} submission={} commandList=0x{:x} color=0x{:x} depth=0x{:x} convertedMV=0x{:x} originalMV=0x{:x} output=0x{:x} input={}x{} outputExtent={}x{} resetHistory={} detail={}",
            stage,
            static_cast<unsigned long long>(diagnostics.frame_id),
            GetCurrentThreadId(),
            static_cast<unsigned long long>(diagnostics.control_generation),
            static_cast<unsigned long long>(diagnostics.device_reset_generation),
            diagnostics.bridge_slot,
            static_cast<unsigned long long>(diagnostics.submission_sequence),
            reinterpret_cast<uintptr_t>(command_list),
            reinterpret_cast<uintptr_t>(params.pColorTexture),
            reinterpret_cast<uintptr_t>(params.pDepthTexture),
            reinterpret_cast<uintptr_t>(params.pVelocityTexture),
            reinterpret_cast<uintptr_t>(diagnostics.original_velocity),
            reinterpret_cast<uintptr_t>(params.pOutputTexture),
            params.inputWidth,
            params.inputHeight,
            diagnostics.output_width,
            diagnostics.output_height,
            params.resetHistory != 0,
            detail);
    };

    if (log_api_call) {
        log_context(spdlog::level::info, "api-enter", "calling public xessD3D12Execute");
    }

    xess_result_t result{};
    NativeExceptionObservation native_exception{};
    try {
        result = invoke_xess_execute(
            m_functions.d3d12_execute,
            m_context,
            command_list,
            &params,
            &native_exception);
    } catch (const std::exception& exception) {
        if (!execute_api_exception_logged.test_and_set(std::memory_order_relaxed)) {
            log_native_exception(native_exception,
                diagnostics.frame_id,
                diagnostics.control_generation,
                diagnostics.device_reset_generation,
                diagnostics.submission_sequence,
                diagnostics.bridge_slot,
                diagnostics.output_width,
                diagnostics.output_height,
                command_list,
                diagnostics.original_velocity,
                params);
            std::string_view what = exception.what() != nullptr ? exception.what() : "";
            if (what.size() > 512) {
                what = what.substr(0, 512);
            }
            log_context(spdlog::level::err, "api-exception", std::string{ "kind=std::exception what=" } + std::string{ what });
        }
        error = "xessD3D12Execute threw std::exception";
        throw;
    } catch (...) {
        if (!execute_api_exception_logged.test_and_set(std::memory_order_relaxed)) {
            log_native_exception(native_exception,
                diagnostics.frame_id,
                diagnostics.control_generation,
                diagnostics.device_reset_generation,
                diagnostics.submission_sequence,
                diagnostics.bridge_slot,
                diagnostics.output_width,
                diagnostics.output_height,
                command_list,
                diagnostics.original_velocity,
                params);
            log_context(spdlog::level::err, "api-exception", "kind=unknown");
        }
        error = "xessD3D12Execute threw an unknown exception";
        throw;
    }

    const bool log_api_failure = result != XESS_RESULT_SUCCESS &&
        !execute_api_failure_logged.test_and_set(std::memory_order_relaxed);
    if (log_api_call || log_api_failure) {
        log_context(result == XESS_RESULT_SUCCESS ? spdlog::level::info : spdlog::level::err, "api-return",
            std::string{ "result=" } + std::to_string(static_cast<int32_t>(result)));
    }
    if (result != XESS_RESULT_SUCCESS) {
        error = result_message("xessD3D12Execute", result);
        m_failure_reason = error;
        return false;
    }

    return true;
}

void RE4XeSSRuntime::shutdown() noexcept {
    if (m_quarantined) {
        return;
    }
    if (m_owner_thread_id.load(std::memory_order_acquire) != 0 && !is_owner_thread()) {
        quarantine();
        return;
    }
    cleanup();
    m_failure_reason.clear();
    m_candidates = {};
    m_selected_path.clear();
    m_runtime_version.reset();
    m_state = State::Unloaded;
    m_sr_initialized = false;
}

void RE4XeSSRuntime::quarantine() noexcept {
    if (m_quarantined) {
        return;
    }

    m_quarantined = true;
    m_sr_initialized = false;
    m_state = State::Faulted;
    m_failure_reason = "XeSS context/module quarantined until process teardown";

    // Intentionally keep the module loaded and lose the raw handles. Calling
    // XeSS from a non-owner thread or unloading code with possible GPU work in
    // flight is less safe than retaining these process-lifetime references.
    m_context = nullptr;
    m_module = nullptr;
    m_functions = {};
}

bool RE4XeSSRuntime::is_owner_thread() const noexcept {
    const auto owner_thread_id = m_owner_thread_id.load(std::memory_order_acquire);
    return owner_thread_id != 0 && GetCurrentThreadId() == owner_thread_id;
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
    if (m_owner_thread_id.load(std::memory_order_acquire) != 0 && !is_owner_thread()) {
        quarantine();
        return;
    }

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
    m_sr_initialized = false;
}

void RE4XeSSRuntime::fail(std::string reason) {
    cleanup();
    m_failure_reason = std::move(reason);
    m_state = State::Faulted;
}

bool RE4XeSSRuntime::bind_or_check_owner_thread(std::string& error) {
    const auto current_thread_id = GetCurrentThreadId();
    DWORD expected_thread_id{};
    if (m_owner_thread_id.compare_exchange_strong(
            expected_thread_id,
            current_thread_id,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        spdlog::info("[RE4XeSS][Init] owner thread established: {}", current_thread_id);
        return true;
    }
    if (expected_thread_id == current_thread_id) {
        return true;
    }

    error = "XeSS API owner-thread violation: expected thread " +
        std::to_string(expected_thread_id) + ", got " + std::to_string(current_thread_id);
    log_owner_thread_violation_once(error);
    return false;
}

bool RE4XeSSRuntime::check_owner_thread(std::string_view operation, std::string& error) const {
    const auto owner_thread_id = m_owner_thread_id.load(std::memory_order_acquire);
    if (owner_thread_id != 0 && owner_thread_id == GetCurrentThreadId()) {
        return true;
    }

    error = "XeSS API owner-thread violation in " + std::string{ operation } +
        ": expected thread " + std::to_string(owner_thread_id) +
        ", got " + std::to_string(GetCurrentThreadId());
    log_owner_thread_violation_once(error);
    return false;
}
