#include "mods/re4_xess/RE4XeSSWorker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include "mods/REFrameworkConfig.hpp"

namespace {

std::string path_for_log(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
}

bool debug_log_enabled() {
    const auto config = REFrameworkConfig::get();
    return config != nullptr && config->is_debug_log_enabled();
}

RE4XeSSWorker::Snapshot dispatch_failure_snapshot(
    int32_t mode,
    uint64_t control_generation,
    uint64_t reset_generation,
    std::string reason) {
    RE4XeSSWorker::Snapshot snapshot{};
    snapshot.draining = true;
    snapshot.faulted = true;
    snapshot.bridge_idle = false;
    snapshot.mode_token = mode;
    snapshot.control_generation = control_generation;
    snapshot.device_reset_generation = reset_generation;
    snapshot.failure_reason = std::move(reason);
    return snapshot;
}

}

RE4XeSSWorker::~RE4XeSSWorker() {
    stop();
}

bool RE4XeSSWorker::start(std::string& error) {
    error.clear();
    std::unique_lock lock{ m_mutex };
    if (m_thread.joinable()) {
        return m_startup_succeeded;
    }

    m_startup_complete = false;
    m_startup_succeeded = false;
    m_stop_requested = false;
    m_worker_exited = false;
    try {
        m_thread = std::thread{ &RE4XeSSWorker::run, this };
    } catch (const std::system_error& exception) {
        error = std::string{ "Could not start the RE4XeSS worker thread: " } + exception.what();
        return false;
    } catch (...) {
        error = "Could not start the RE4XeSS worker thread";
        return false;
    }

    m_startup_ready.wait(lock, [this] { return m_startup_complete; });
    if (!m_startup_succeeded) {
        error = "The RE4XeSS worker failed during startup";
        lock.unlock();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        return false;
    }
    return true;
}

RE4XeSSWorker::ControlResult RE4XeSSWorker::service_sync(ControlRequest request) {
    if (request.caller_thread_id == 0) {
        request.caller_thread_id = GetCurrentThreadId();
    }
    const auto requested_mode = request.mode_token;
    const auto control_generation = request.control_generation;
    const auto reset_generation = request.device_reset_generation;

    auto response = dispatch_sync(Request{ std::move(request) });
    if (response && std::holds_alternative<ControlResult>(*response)) {
        return std::get<ControlResult>(std::move(*response));
    }

    ControlResult failed{};
    failed.status = ServiceStatus::DispatchFailed;
    failed.control_generation = control_generation;
    failed.device_reset_generation = reset_generation;
    failed.snapshot = dispatch_failure_snapshot(
        requested_mode,
        control_generation,
        reset_generation,
        "The RE4XeSS worker is unavailable or already has an outstanding operation");
    return failed;
}

RE4XeSSWorker::SubmitResult RE4XeSSWorker::submit_sync(SubmitRequest request) {
    if (request.caller_thread_id == 0) {
        request.caller_thread_id = GetCurrentThreadId();
    }
    const auto mode_token = request.mode_token;
    const auto control_generation = request.control_generation;
    const auto reset_generation = request.device_reset_generation;
    auto response = dispatch_sync(Request{ std::move(request) });
    if (response && std::holds_alternative<SubmitResult>(*response)) {
        return std::get<SubmitResult>(std::move(*response));
    }

    SubmitResult failed{};
    failed.status = SubmitResult::Status::DispatchFailed;
    failed.control_generation = control_generation;
    failed.device_reset_generation = reset_generation;
    failed.snapshot = dispatch_failure_snapshot(
        mode_token,
        control_generation,
        reset_generation,
        "The RE4XeSS worker is unavailable or already has an outstanding operation");
    failed.bridge_idle = failed.snapshot.bridge_idle;
    failed.bridge_quarantined = failed.snapshot.bridge_quarantined;
    failed.bridge_device_removed = failed.snapshot.bridge_device_removed;
    failed.failure_reason = "The RE4XeSS worker is unavailable or already has an outstanding operation";
    return failed;
}

void RE4XeSSWorker::stop() noexcept {
    {
        std::lock_guard lock{ m_mutex };
        if (!m_thread.joinable()) {
            return;
        }
        m_stop_requested = true;
        m_request_available.notify_one();
    }

    if (m_thread.get_id() == std::this_thread::get_id()) {
        spdlog::error("[RE4XeSS][Worker] stop requested from the worker thread; refusing self-join");
        return;
    }
    m_thread.join();
}

DWORD RE4XeSSWorker::thread_id() const noexcept {
    return m_thread_id.load(std::memory_order_acquire);
}

std::optional<RE4XeSSWorker::Response> RE4XeSSWorker::dispatch_sync(Request request) {
    std::unique_lock lock{ m_mutex };
    if (!m_startup_succeeded || m_stop_requested || m_worker_exited || m_operation_in_flight ||
        !std::holds_alternative<std::monostate>(m_pending_request)) {
        return std::nullopt;
    }

    m_operation_in_flight = true;
    const auto sequence = ++m_next_sequence;
    m_pending_request = std::move(request);
    m_request_available.notify_one();
    m_response_ready.wait(lock, [this, sequence] {
        return m_completed_sequence == sequence || m_worker_exited;
    });

    std::optional<Response> result;
    if (m_completed_sequence == sequence && !std::holds_alternative<std::monostate>(m_completed_response)) {
        result.emplace(std::move(m_completed_response));
        m_completed_response = std::monostate{};
    }
    m_operation_in_flight = false;
    return result;
}

void RE4XeSSWorker::run() noexcept {
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    const auto kernel_module = GetModuleHandleW(L"Kernel32.dll");
    const auto set_thread_description = kernel_module != nullptr
        ? reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel_module, "SetThreadDescription"))
        : nullptr;
    if (set_thread_description != nullptr) {
        (void)set_thread_description(GetCurrentThread(), L"RE4XeSS Worker");
    }
    const auto worker_id = GetCurrentThreadId();
    m_thread_id.store(worker_id, std::memory_order_release);
    spdlog::info("[RE4XeSS][Worker] started thread={}", worker_id);
    try {
        m_runtime = std::make_unique<RE4XeSSRuntime>();
        m_bridge = std::make_unique<RE4XeSSD3D12>();
    } catch (...) {
        m_runtime.reset();
        m_bridge.reset();
        std::lock_guard lock{ m_mutex };
        m_startup_succeeded = false;
        m_startup_complete = true;
        m_worker_exited = true;
        m_startup_ready.notify_all();
        m_response_ready.notify_all();
        m_thread_id.store(0, std::memory_order_release);
        spdlog::error("[RE4XeSS][Worker] failed to initialize worker-owned state");
        return;
    }
    {
        std::lock_guard lock{ m_mutex };
        m_startup_succeeded = true;
        m_startup_complete = true;
        m_startup_ready.notify_all();
    }

    for (;;) {
        Request request{};
        uint64_t sequence{};
        {
            std::unique_lock lock{ m_mutex };
            m_request_available.wait(lock, [this] {
                return !std::holds_alternative<std::monostate>(m_pending_request) || m_stop_requested;
            });
            if (!std::holds_alternative<std::monostate>(m_pending_request)) {
                request = std::move(m_pending_request);
                m_pending_request = std::monostate{};
                sequence = m_next_sequence;
            } else if (m_stop_requested) {
                lock.unlock();
                shutdown_owned_state();
                m_bridge.reset();
                m_runtime.reset();
                lock.lock();
                m_worker_exited = true;
                m_response_ready.notify_all();
                m_startup_ready.notify_all();
                break;
            }
        }

        Response response{};
        try {
            response = process_request(request);
        } catch (const std::exception& exception) {
            quarantine_after_unhandled_exception();
            const auto reason = std::string{ "Unhandled worker exception: " } + exception.what();
            if (std::holds_alternative<ControlRequest>(request)) {
                const auto& control = std::get<ControlRequest>(request);
                ControlResult failed{};
                failed.status = ServiceStatus::Faulted;
                failed.control_generation = control.control_generation;
                failed.device_reset_generation = control.device_reset_generation;
                failed.snapshot = make_snapshot(control.mode_token, control.control_generation,
                    control.device_reset_generation, false, true, reason);
                response = std::move(failed);
            } else if (std::holds_alternative<SubmitRequest>(request)) {
                auto failed = make_submit_result(std::get<SubmitRequest>(request), SubmitResult::Status::Faulted, reason);
                response = std::move(failed);
            }
        } catch (...) {
            quarantine_after_unhandled_exception();
            constexpr auto reason = "Unknown exception in the RE4XeSS worker";
            if (std::holds_alternative<ControlRequest>(request)) {
                const auto& control = std::get<ControlRequest>(request);
                ControlResult failed{};
                failed.status = ServiceStatus::Faulted;
                failed.control_generation = control.control_generation;
                failed.device_reset_generation = control.device_reset_generation;
                failed.snapshot = make_snapshot(control.mode_token, control.control_generation,
                    control.device_reset_generation, false, true, reason);
                response = std::move(failed);
            } else if (std::holds_alternative<SubmitRequest>(request)) {
                auto failed = make_submit_result(std::get<SubmitRequest>(request), SubmitResult::Status::Faulted, reason);
                response = std::move(failed);
            }
        }

        {
            std::lock_guard lock{ m_mutex };
            m_completed_response = std::move(response);
            m_completed_sequence = sequence;
            m_response_ready.notify_all();
        }
    }

    m_thread_id.store(0, std::memory_order_release);
    spdlog::info("[RE4XeSS][Worker] shutdown thread={}", worker_id);
}

RE4XeSSWorker::Response RE4XeSSWorker::process_request(Request request) {
    if (std::holds_alternative<ControlRequest>(request)) {
        return process_control(std::get<ControlRequest>(request));
    }
    if (std::holds_alternative<SubmitRequest>(request)) {
        return process_submit(std::get<SubmitRequest>(std::move(request)));
    }
    return std::monostate{};
}

RE4XeSSWorker::ControlResult RE4XeSSWorker::process_control(const ControlRequest& request) {
    if (debug_log_enabled() && m_service_log_count < 32) {
        ++m_service_log_count;
        spdlog::info("[RE4XeSS][Worker] service callerThread={} workerThread={} controlGeneration={} resetGeneration={}",
            request.caller_thread_id,
            thread_id(),
            static_cast<unsigned long long>(request.control_generation),
            static_cast<unsigned long long>(request.device_reset_generation));
    }

    const auto result = [this, &request](ServiceStatus status, bool ready, bool draining, bool faulted, std::string reason = {}, bool removed = false) {
        ControlResult control_result{};
        control_result.status = status;
        control_result.ready_for_frame = ready;
        control_result.control_generation = request.control_generation;
        control_result.device_reset_generation = request.device_reset_generation;
        control_result.snapshot = make_snapshot(
            request.mode_token,
            request.control_generation,
            request.device_reset_generation,
            draining,
            faulted,
            std::move(reason),
            removed);
        return control_result;
    };

    if (m_terminal_faulted) {
        return result(ServiceStatus::Faulted, false, false, true, m_terminal_failure_reason);
    }
    if (request.control_generation < m_last_accepted_control_generation ||
        request.device_reset_generation < m_last_accepted_device_reset_generation) {
        return result(ServiceStatus::Stale, false, false, false, "Control request generation is older than the worker's accepted generation");
    }
    m_last_accepted_control_generation = request.control_generation;
    m_last_accepted_device_reset_generation = request.device_reset_generation;

    if (!request.external_fault.empty()) {
        mark_execution_fault(request.external_fault, request.control_generation, request.device_reset_generation);
    }

    std::string owner_error;
    if (!m_runtime->bind_owner_thread(owner_error)) {
        mark_execution_fault(owner_error, request.control_generation, request.device_reset_generation);
        return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
    }

    auto* device = request.device.Get();
    auto* queue = request.queue.Get();
    const bool has_quality = request.quality.has_value();
    const bool active = request.active && has_quality;
    const bool has_display = active && request.display.x != 0 && request.display.y != 0;
    const bool queue_is_direct = queue != nullptr && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    const bool has_d3d_state = active && device != nullptr && queue_is_direct && has_display;
    const std::string unavailable_reason = queue != nullptr && !queue_is_direct
        ? "The active RE4 D3D12 command queue is not DIRECT"
        : "Waiting for a valid RE4 D3D12 device, DIRECT queue, and display extent";

    const auto reset_generation = request.device_reset_generation;
    const bool reset_pending = reset_generation != m_owner_device_reset_generation;
    const auto poll_result = m_bridge->has_generation()
        ? m_bridge->poll()
        : RE4XeSSD3D12::PollResult::Idle;
    m_last_bridge_device_removed = false;
    auto effective_poll_result = poll_result;
    if (poll_result == RE4XeSSD3D12::PollResult::DeviceRemoved) {
        auto* removed_device = m_owner_configuration.device.Get();
        m_bridge->shutdown_after_device_removed();
        m_runtime->shutdown();
        m_owner_configuration = {};
        m_owner_waiting_for_new_device = true;
        m_removed_device_identity = removed_device;
        m_owner_device_reset_generation = reset_generation;
        m_last_bridge_device_removed = true;
        spdlog::error("[RE4XeSS][Failure] old D3D12 generation is terminal after confirmed device removal; XeSS context destroyed on worker thread");
        if (!reset_pending && device == removed_device) {
            return result(ServiceStatus::Draining, false, true, false,
                "Waiting for a replacement D3D12 device after confirmed device removal", true);
        }
        effective_poll_result = RE4XeSSD3D12::PollResult::Idle;
    }

    if (m_owner_waiting_for_new_device) {
        if (!reset_pending && device == m_removed_device_identity) {
            return result(ServiceStatus::Draining, false, true, false,
                "Waiting for a replacement D3D12 device after confirmed device removal", m_last_bridge_device_removed);
        }
        m_owner_waiting_for_new_device = false;
        m_removed_device_identity = nullptr;
    }

    const bool old_configuration_differs = m_owner_configuration.valid &&
        (!has_d3d_state ||
            m_owner_configuration.device.Get() != device ||
            m_owner_configuration.queue.Get() != queue ||
            m_owner_configuration.mode_token != request.mode_token ||
            !has_quality || m_owner_configuration.quality != *request.quality ||
            m_owner_configuration.display.x != request.display.x ||
            m_owner_configuration.display.y != request.display.y ||
            m_owner_configuration.control_generation != request.control_generation ||
            m_owner_configuration.device_reset_generation != request.device_reset_generation);
    const bool transition_pending = !active || reset_pending || old_configuration_differs ||
        (active && !has_d3d_state) || m_owner_execution_faulted;

    if (transition_pending) {
        if (m_bridge->has_generation()) {
            if (effective_poll_result == RE4XeSSD3D12::PollResult::InFlight) {
                return result(ServiceStatus::Draining, false, true, false,
                    "XeSS bridge work is draining before reconfiguration", m_last_bridge_device_removed);
            }
            if (effective_poll_result == RE4XeSSD3D12::PollResult::Quarantined) {
                return result(ServiceStatus::Faulted, false, true, true, m_bridge->failure_reason(), m_last_bridge_device_removed);
            }
            if (effective_poll_result != RE4XeSSD3D12::PollResult::Idle) {
                mark_execution_fault(m_bridge->failure_reason(), request.control_generation, reset_generation);
                return result(ServiceStatus::Faulted, false, true, true, m_owner_failure_reason, m_last_bridge_device_removed);
            }
            m_bridge->shutdown();
        }

        if (reset_pending || !active || !has_d3d_state || old_configuration_differs || m_owner_execution_faulted) {
            m_runtime->shutdown();
        }
        m_owner_configuration = {};

        if (reset_pending) {
            m_owner_device_reset_generation = reset_generation;
        }

        if (m_owner_execution_faulted) {
            m_runtime->shutdown();
            const bool explicit_retry =
                request.control_generation != m_owner_fault_control_generation ||
                reset_generation != m_owner_fault_device_reset_generation;
            if (!explicit_retry) {
                return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason, m_last_bridge_device_removed);
            }
            m_owner_execution_faulted = false;
            m_owner_failure_reason.clear();
        }

        if (!active) {
            m_runtime->shutdown();
            return result(ServiceStatus::Waiting, false, false, false, "Off - native RE4 rendering", m_last_bridge_device_removed);
        }

        if (!has_d3d_state) {
            m_runtime->shutdown();
            return result(ServiceStatus::Waiting, false, true, false, unavailable_reason, m_last_bridge_device_removed);
        }
    }

    if (!has_d3d_state) {
        return result(ServiceStatus::Waiting, false, false, false, unavailable_reason, m_last_bridge_device_removed);
    }

    if (m_owner_configuration.valid) {
        const RE4XeSSD3D12::Signature bridge_signature{
            m_owner_configuration.input.optimal.x,
            m_owner_configuration.input.optimal.y,
            m_owner_configuration.display.x,
            m_owner_configuration.display.y,
            DXGI_FORMAT_R11G11B10_FLOAT,
        };
        if (m_bridge->has_generation() && !m_bridge->matches(bridge_signature, device, queue)) {
            if (effective_poll_result == RE4XeSSD3D12::PollResult::InFlight ||
                effective_poll_result == RE4XeSSD3D12::PollResult::Quarantined) {
                return result(ServiceStatus::Draining, false, true,
                    effective_poll_result == RE4XeSSD3D12::PollResult::Quarantined,
                    "XeSS bridge generation is draining before resource recreation", m_last_bridge_device_removed);
            }
            if (effective_poll_result == RE4XeSSD3D12::PollResult::DeviceRemoved) {
                return result(ServiceStatus::Faulted, false, true, true, m_bridge->failure_reason(), true);
            }
            m_bridge->shutdown();
            m_runtime->shutdown();
            m_owner_configuration = {};
            return result(ServiceStatus::Draining, false, true, false,
                "XeSS bridge signature changed; recreating the producer context", m_last_bridge_device_removed);
        }

        return result(ServiceStatus::Ready, true, false, false, {}, m_last_bridge_device_removed);
    }

    if (m_runtime->state() != RE4XeSSRuntime::State::ContextReady) {
        if (request.reframework_directory.empty()) {
            mark_execution_fault("Could not resolve the directory containing the REFramework module",
                request.control_generation, reset_generation);
            return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
        }

        if (!m_runtime->initialize(device, request.reframework_directory)) {
            mark_execution_fault(m_runtime->failure_reason(), request.control_generation, reset_generation);
            return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
        }

        const auto& candidates = m_runtime->candidates();
        spdlog::info("[RE4XeSS][Runtime] REFramework directory='{}'; candidate 1='{}'; candidate 2='{}'; selected='{}'; load result=success",
            path_for_log(request.reframework_directory),
            path_for_log(candidates[0]),
            path_for_log(candidates[1]),
            path_for_log(m_runtime->selected_path()));
    }

    std::string query_error;
    const auto input = m_runtime->query_optimal_input_resolution(request.display, *request.quality, query_error);
    if (!input) {
        mark_execution_fault(query_error, request.control_generation, reset_generation);
        m_runtime->shutdown();
        return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
    }

    const auto render = input->optimal;
    const auto cross_a = static_cast<uint64_t>(render.x) * request.display.y;
    const auto cross_b = static_cast<uint64_t>(render.y) * request.display.x;
    const auto cross_error = cross_a > cross_b ? cross_a - cross_b : cross_b - cross_a;
    const auto relative_aspect_error = render.y != 0 && request.display.x != 0
        ? static_cast<double>(cross_error) / (static_cast<double>(render.y) * request.display.x)
        : std::numeric_limits<double>::infinity();
    if (render.x == 0 || render.y == 0 || render.x > request.display.x || render.y > request.display.y ||
        !std::isfinite(relative_aspect_error) || relative_aspect_error > 0.01) {
        mark_execution_fault("XeSS producer returned an invalid or aspect-incompatible optimal input extent",
            request.control_generation, reset_generation);
        m_runtime->shutdown();
        return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
    }

    const RE4XeSSRuntime::InitSignature init_signature{
        request.display,
        *request.quality,
        XESS_INIT_FLAG_INVERTED_DEPTH,
        static_cast<float>(render.x) / 2.0f,
        -static_cast<float>(render.y) / 2.0f,
    };
    std::string init_error;
    if (!m_runtime->initialize_sr(init_signature, init_error)) {
        mark_execution_fault(init_error, request.control_generation, reset_generation);
        m_runtime->shutdown();
        return result(ServiceStatus::Faulted, false, false, true, m_owner_failure_reason);
    }

    m_owner_configuration = {
        request.device,
        request.queue,
        request.mode_token,
        *request.quality,
        request.display,
        *input,
        request.control_generation,
        reset_generation,
        true,
    };
    m_owner_device_reset_generation = reset_generation;
    m_last_bridge_device_removed = false;
    spdlog::info(
        "[RE4XeSS][Init] ownerThread={} device=0x{:x} queue=0x{:x} queueType={} display={}x{} render={}x{} mode={} quality={} initFlags={} velocityScale=({:.1f},{:.1f}) convertedMV={} output={} ring={}",
        m_runtime->owner_thread_id(),
        reinterpret_cast<uintptr_t>(device),
        reinterpret_cast<uintptr_t>(queue),
        static_cast<uint32_t>(queue->GetDesc().Type),
        request.display.x,
        request.display.y,
        render.x,
        render.y,
        request.mode_token,
        static_cast<int32_t>(*request.quality),
        XESS_INIT_FLAG_INVERTED_DEPTH,
        init_signature.velocity_scale_x,
        init_signature.velocity_scale_y,
        static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<uint32_t>(DXGI_FORMAT_R11G11B10_FLOAT),
        RE4XeSSD3D12::SLOT_COUNT);

    return result(ServiceStatus::Waiting, false, false, false,
        "XeSS runtime initialized; first control pass does not submit a frame");
}

RE4XeSSWorker::SubmitResult RE4XeSSWorker::process_submit(SubmitRequest request) {
    if (debug_log_enabled() && m_submit_log_count < 32) {
        ++m_submit_log_count;
        spdlog::info("[RE4XeSS][Worker] submit callerThread={} workerThread={} controlGeneration={} resetGeneration={}",
            request.caller_thread_id,
            thread_id(),
            static_cast<unsigned long long>(request.control_generation),
            static_cast<unsigned long long>(request.device_reset_generation));
    }

    if (request.control_generation != m_last_accepted_control_generation ||
        request.device_reset_generation != m_last_accepted_device_reset_generation ||
        !m_owner_configuration.valid ||
        request.control_generation != m_owner_configuration.control_generation ||
        request.device_reset_generation != m_owner_configuration.device_reset_generation ||
        request.bridge_signature.render_width != m_owner_configuration.input.optimal.x ||
        request.bridge_signature.render_height != m_owner_configuration.input.optimal.y ||
        request.bridge_signature.display_width != m_owner_configuration.display.x ||
        request.bridge_signature.display_height != m_owner_configuration.display.y ||
        request.bridge_signature.color_format != DXGI_FORMAT_R11G11B10_FLOAT) {
        return make_submit_result(request, SubmitResult::Status::Stale,
            "Submit request generation or bridge signature does not match the worker's accepted runtime configuration");
    }
    if (m_terminal_faulted || m_owner_execution_faulted || !m_runtime->sr_initialized()) {
        return make_submit_result(request, SubmitResult::Status::Faulted,
            m_terminal_failure_reason.empty() ? m_owner_failure_reason : m_terminal_failure_reason);
    }
    if (request.color_pin == nullptr || request.depth_pin == nullptr || request.velocity_pin == nullptr ||
        request.output_pin == nullptr || request.output.resource == nullptr) {
        return make_submit_result(request, SubmitResult::Status::Faulted,
            "Worker submit request is missing one or more pinned resources");
    }

    request.frame.color = request.color_pin.Get();
    request.frame.depth = request.depth_pin.Get();
    request.frame.velocity = request.velocity_pin.Get();
    request.output.resource = request.output_pin.Get();

    const auto& configuration = m_owner_configuration;
    const auto current_poll = m_bridge->has_generation()
        ? m_bridge->poll()
        : RE4XeSSD3D12::PollResult::Idle;
    if (m_bridge->has_generation() && !m_bridge->matches(
            request.bridge_signature,
            configuration.device.Get(),
            configuration.queue.Get())) {
        if (current_poll == RE4XeSSD3D12::PollResult::InFlight ||
            current_poll == RE4XeSSD3D12::PollResult::Quarantined) {
            return make_submit_result(request, SubmitResult::Status::Busy,
                "Previous bridge generation is still draining");
        }
        if (current_poll == RE4XeSSD3D12::PollResult::DeviceRemoved) {
            m_last_bridge_device_removed = true;
            return make_submit_result(request, SubmitResult::Status::Faulted,
                "The previous bridge device generation was removed");
        }
        if (current_poll != RE4XeSSD3D12::PollResult::Idle) {
            mark_execution_fault(m_bridge->failure_reason(), request.control_generation, request.device_reset_generation);
            return make_submit_result(request, SubmitResult::Status::Faulted, m_owner_failure_reason);
        }
        m_bridge->shutdown();
    }

    if (!m_bridge->ready()) {
        std::string bridge_error;
        if (!m_bridge->initialize(
                configuration.device.Get(),
                configuration.queue.Get(),
                request.bridge_signature,
                bridge_error)) {
            mark_execution_fault(bridge_error, request.control_generation, request.device_reset_generation);
            return make_submit_result(request, SubmitResult::Status::Faulted, m_owner_failure_reason);
        }
    }

    std::string submit_error;
    RE4XeSSD3D12::SubmissionInfo submission_info{};
    const auto submit_result = m_bridge->submit(
        request.frame,
        *m_runtime,
        request.output,
        request.control_generation,
        request.device_reset_generation,
        submission_info,
        submit_error);
    const auto attach_submission_info = [&] (SubmitResult& result) {
        result.trace_id = request.frame.lifetime_trace_id;
        result.submit_ordinal = submission_info.submission_ordinal;
        result.writer_fence_value = submission_info.writer_fence_value;
        result.bridge_slot = submission_info.slot;
        result.execute_api_succeeded = submission_info.execute_api_succeeded;
        result.queue_submitted = submission_info.command_lists_submitted;
        result.writer_signal_succeeded = submission_info.writer_signal_succeeded;
    };
    if (submit_result == RE4XeSSD3D12::SubmitResult::Submitted) {
        auto result = make_submit_result(request, SubmitResult::Status::Submitted);
        attach_submission_info(result);
        return result;
    }
    if (submit_result == RE4XeSSD3D12::SubmitResult::Busy) {
        auto result = make_submit_result(request, SubmitResult::Status::Busy, std::move(submit_error));
        attach_submission_info(result);
        return result;
    }

    mark_execution_fault(submit_error.empty() ? m_bridge->failure_reason() : submit_error,
        request.control_generation, request.device_reset_generation);
    auto result = make_submit_result(request, SubmitResult::Status::Faulted, m_owner_failure_reason);
    attach_submission_info(result);
    return result;
}

RE4XeSSWorker::SubmitResult RE4XeSSWorker::make_submit_result(
    const SubmitRequest& request,
    SubmitResult::Status status,
    std::string reason) {
    SubmitResult result{};
    result.status = status;
    result.control_generation = request.control_generation;
    result.device_reset_generation = request.device_reset_generation;
    result.snapshot = make_snapshot(
        request.mode_token,
        request.control_generation,
        request.device_reset_generation,
        status == SubmitResult::Status::Busy,
        status == SubmitResult::Status::Faulted,
        reason,
        m_last_bridge_device_removed);
    result.bridge_idle = result.snapshot.bridge_idle;
    result.bridge_quarantined = result.snapshot.bridge_quarantined;
    result.bridge_device_removed = result.snapshot.bridge_device_removed;
    result.failure_reason = std::move(reason);
    return result;
}

RE4XeSSWorker::Snapshot RE4XeSSWorker::make_snapshot(
    int32_t requested_mode,
    uint64_t control_generation,
    uint64_t device_reset_generation,
    bool draining,
    bool faulted,
    std::string reason,
    bool device_removed) const {
    Snapshot snapshot{};
    snapshot.context_ready = m_runtime->state() == RE4XeSSRuntime::State::ContextReady;
    snapshot.execution_ready = m_bridge->ready();
    snapshot.draining = draining;
    snapshot.faulted = faulted || m_terminal_faulted || m_owner_execution_faulted ||
        m_runtime->state() == RE4XeSSRuntime::State::Faulted || m_bridge->quarantined();
    snapshot.bridge_idle = !m_bridge->has_generation() || m_bridge->idle();
    snapshot.bridge_quarantined = m_bridge->quarantined();
    snapshot.bridge_device_removed = device_removed || m_last_bridge_device_removed;
    snapshot.mode_token = m_owner_configuration.valid ? m_owner_configuration.mode_token : requested_mode;
    snapshot.quality = m_owner_configuration.quality;
    snapshot.display = m_owner_configuration.display;
    snapshot.input = m_owner_configuration.input;
    snapshot.device_identity = reinterpret_cast<uintptr_t>(m_owner_configuration.device.Get());
    snapshot.queue_identity = reinterpret_cast<uintptr_t>(m_owner_configuration.queue.Get());
    snapshot.control_generation = control_generation;
    snapshot.device_reset_generation = device_reset_generation;
    if (reason.empty()) {
        if (m_terminal_faulted) {
            reason = m_terminal_failure_reason;
        } else if (m_owner_execution_faulted) {
            reason = m_owner_failure_reason;
        } else if (m_runtime->state() == RE4XeSSRuntime::State::Faulted) {
            reason = m_runtime->failure_reason();
        } else if (m_bridge->quarantined()) {
            reason = m_bridge->failure_reason();
        }
    }
    snapshot.failure_reason = std::move(reason);
    return snapshot;
}

void RE4XeSSWorker::mark_execution_fault(
    std::string reason,
    uint64_t control_generation,
    uint64_t device_reset_generation) {
    m_owner_execution_faulted = true;
    m_owner_fault_control_generation = control_generation;
    m_owner_fault_device_reset_generation = device_reset_generation;
    m_owner_failure_reason = std::move(reason);
    spdlog::error("[RE4XeSS][Failure] {}", m_owner_failure_reason);
}

void RE4XeSSWorker::shutdown_owned_state() noexcept {
    if (m_bridge == nullptr || m_runtime == nullptr) {
        m_owner_configuration = {};
        return;
    }
    const auto result = m_bridge->has_generation()
        ? m_bridge->poll()
        : RE4XeSSD3D12::PollResult::Idle;
    if (result == RE4XeSSD3D12::PollResult::DeviceRemoved) {
        m_bridge->shutdown_after_device_removed();
        m_runtime->shutdown();
    } else if (result == RE4XeSSD3D12::PollResult::Idle) {
        m_bridge->shutdown();
        m_runtime->shutdown();
    } else {
        m_bridge->quarantine();
        m_runtime->quarantine();
        spdlog::warn("[RE4XeSS][Worker] shutdown quarantined an unfinished bridge generation; no GPU fence wait was performed");
    }
    m_owner_configuration = {};
}

void RE4XeSSWorker::quarantine_after_unhandled_exception() noexcept {
    m_terminal_faulted = true;
    m_terminal_failure_reason = "RE4XeSS worker terminated an operation after an unhandled exception";
    if (m_bridge != nullptr) {
        m_bridge->quarantine();
    }
    if (m_runtime != nullptr) {
        m_runtime->quarantine();
    }
    spdlog::error("[RE4XeSS][Worker] {}", m_terminal_failure_reason);
}
