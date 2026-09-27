#include "mods/re4_xess/RE4XeSSOutputHandoff.hpp"

#include <array>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "mods/REFrameworkConfig.hpp"

namespace {

constexpr auto OUTPUT_READ_STATE = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
constexpr auto REQUIRED_OUTPUT_FLAGS = static_cast<D3D12_RESOURCE_FLAGS>(
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

struct RetainedOutputGeneration {
    sdk::intrusive_ptr<sdk::renderer::TargetState> handoff_state{};
    sdk::intrusive_ptr<sdk::renderer::TargetState> saved_original_state{};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource{};
    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence{};
};

std::string retirement_status_message(RE4XeSSOutputHandoff::RetirementStatus status) {
    switch (status) {
    case RE4XeSSOutputHandoff::RetirementStatus::NoGeneration: return "no previous output generation";
    case RE4XeSSOutputHandoff::RetirementStatus::Active: return "output generation is active";
    case RE4XeSSOutputHandoff::RetirementStatus::WriterPending: return "bridge writer fence is still draining";
    case RE4XeSSOutputHandoff::RetirementStatus::MissingMarker: return "waiting for a same-generation downstream retirement marker";
    case RE4XeSSOutputHandoff::RetirementStatus::Draining: return "downstream retirement marker is queued and draining";
    case RE4XeSSOutputHandoff::RetirementStatus::Ready: return "previous output generation retired";
    case RE4XeSSOutputHandoff::RetirementStatus::Quarantined: return "output generation is quarantined";
    case RE4XeSSOutputHandoff::RetirementStatus::DeviceRemoved: return "old D3D12 device generation was removed";
    }
    return "unknown output retirement state";
}

bool valid_texture_extent(
    const D3D12_RESOURCE_DESC& description,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format) {
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.Width == width && description.Height == height &&
        description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
        description.Format == format;
}

}

RE4XeSSOutputHandoff::~RE4XeSSOutputHandoff() {
    if (m_handoff_state != nullptr || m_device != nullptr || m_queue != nullptr || m_retirement_fence != nullptr) {
        preserve_quarantined_generation();
    }
}

bool RE4XeSSOutputHandoff::restore(
    sdk::renderer::layer::Overlay* layer,
    DWORD current_thread,
    std::string& error) {
    error.clear();
    if (!m_installed) {
        return true;
    }
    if (current_thread != m_owner_thread_id) {
        error = "Overlay handoff restoration was requested from a non-owner thread";
        quarantine(error);
        return false;
    }

    if (layer == nullptr) {
        error = "Overlay layer is unavailable; the installed handoff remains retained for a later owner-thread restore";
        return false;
    }
    if (layer != m_installed_overlay) {
        {
            std::lock_guard lock{ m_retirement_mutex };
            m_installed = false;
            m_installed_snapshot.store(false, std::memory_order_release);
            m_installed_overlay = nullptr;
            m_retirement_requested = true;
            m_missing_marker = m_marker_pending;
            m_retirement_status = m_marker_pending
                ? RetirementStatus::MissingMarker
                : RetirementStatus::Draining;
            m_failure_reason = "Overlay generation changed while a handoff was installed; old Overlay was not dereferenced";
        }
        spdlog::warn("[RE4XeSS][Output] Overlay generation changed; old target state remains retained for retirement");
        return true;
    }

    auto& main_state = layer->get_main_target_state();
    if (main_state.get() == m_handoff_state.get()) {
        main_state = m_saved_original_state;
    } else if (main_state.get() != m_saved_original_state.get()) {
        error = "Overlay main TargetState changed to an unexpected object while the XeSS handoff was installed";
        quarantine(error);
        return false;
    }

    bool log_missing_marker{};
    uint64_t missing_marker_frame{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_installed = false;
        m_installed_snapshot.store(false, std::memory_order_release);
        m_installed_overlay = nullptr;
        if (m_marker_pending) {
            m_missing_marker = true;
            m_retirement_status = RetirementStatus::MissingMarker;
            if (m_failure_reason.empty()) {
                m_failure_reason = "The previous installed handoff frame has no post-Present retirement marker yet";
            }
            m_marker_wait_logged = true;
            if (m_delayed_marker_log_count < 8) {
                ++m_delayed_marker_log_count;
                log_missing_marker = true;
                missing_marker_frame = m_installed_frame;
            }
        } else if (m_retirement_requested) {
            m_retirement_status = RetirementStatus::Draining;
        }
    }
    if (log_missing_marker) {
        spdlog::warn("[RE4XeSS][Output] restored handoff frame={} without its post-Present marker; next submission/install is blocked until same-generation settlement",
            static_cast<unsigned long long>(missing_marker_frame));
    }
    bool log_restore{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        log_restore = REFrameworkConfig::get()->is_debug_log_enabled() && m_restore_log_count < 32;
        if (log_restore) {
            ++m_restore_log_count;
        }
    }
    if (log_restore) {
        spdlog::info("[RE4XeSS][Output] restored original Overlay main state before frame resource collection; frame={}",
            static_cast<unsigned long long>(m_installed_frame));
    }
    return true;
}

void RE4XeSSOutputHandoff::request_retirement(std::string_view reason) {
    bool changed{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (m_handoff_state == nullptr) {
            return;
        }
        changed = !m_retirement_requested;
        m_retirement_requested = true;
        if (m_retirement_status == RetirementStatus::Active) {
            m_retirement_status = m_marker_pending
                ? RetirementStatus::MissingMarker
                : RetirementStatus::Draining;
        }
        if (changed && !reason.empty()) {
            m_failure_reason = std::string{ reason };
        }
    }
    if (changed) {
        spdlog::info("[RE4XeSS][Output] retirement requested: {}", reason.empty() ? "generation transition" : reason);
    }
}

RE4XeSSOutputHandoff::RetirementStatus RE4XeSSOutputHandoff::poll_retirement(
    bool bridge_writer_idle,
    bool bridge_device_removed) {
    if (m_handoff_state == nullptr) {
        return RetirementStatus::NoGeneration;
    }

    RetirementStatus status{};
    bool release{};
    bool log_status{};
    std::string status_reason;
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (!m_retirement_requested) {
            return RetirementStatus::Active;
        }
        if (m_installed) {
            m_hard_quarantined = true;
            m_failure_reason = "Refusing to retire an output TargetState while it is still installed in Overlay";
        }

        if (confirmed_device_removal(bridge_device_removed)) {
            m_device_removed = true;
            m_hard_quarantined = false;
            m_bridge_writer_uncertain = false;
            m_marker_pending = false;
            m_installed = false;
            m_installed_snapshot.store(false, std::memory_order_release);
            m_installed_overlay = nullptr;
            status = RetirementStatus::DeviceRemoved;
            release = true;
        } else {
            bool writer_quarantine_pending{};
            if (m_bridge_writer_uncertain) {
                if (!bridge_writer_idle) {
                    writer_quarantine_pending = true;
                    status = RetirementStatus::Quarantined;
                    m_failure_reason = "XeSS writer completion remains quarantined by the bridge generation";
                } else {
                    m_bridge_writer_uncertain = false;
                    m_failure_reason.clear();
                }
            }

            if (!writer_quarantine_pending) {
                if (m_hard_quarantined) {
                    status = RetirementStatus::Quarantined;
                } else if (!bridge_writer_idle) {
                    status = RetirementStatus::WriterPending;
                } else if (m_installed) {
                    m_hard_quarantined = true;
                    m_failure_reason = "Refusing to retire an output TargetState while it is still installed in Overlay";
                    status = RetirementStatus::Quarantined;
                } else if (!m_downstream_use_seen) {
                    status = RetirementStatus::Ready;
                    release = true;
                } else if (m_marker_pending || m_last_signaled_retirement_value == 0) {
                    m_missing_marker = true;
                    status = RetirementStatus::MissingMarker;
                } else if (m_retirement_fence == nullptr) {
                    m_hard_quarantined = true;
                    m_failure_reason = "Downstream retirement fence disappeared before lifetime proof";
                    status = RetirementStatus::Quarantined;
                } else {
                    const auto completed = m_retirement_fence->GetCompletedValue();
                    if (completed == std::numeric_limits<uint64_t>::max()) {
                        if (confirmed_device_removal(false)) {
                            m_device_removed = true;
                            status = RetirementStatus::DeviceRemoved;
                            release = true;
                        } else {
                            m_hard_quarantined = true;
                            m_failure_reason = "Retirement fence returned UINT64_MAX without confirmed device removal";
                            status = RetirementStatus::Quarantined;
                        }
                    } else {
                        m_last_completed_retirement_value = completed;
                        if (completed >= m_last_signaled_retirement_value) {
                            status = RetirementStatus::Ready;
                            release = true;
                        } else {
                            status = RetirementStatus::Draining;
                        }
                    }
                }
            }
        }
        m_retirement_status = status;
        status_reason = m_failure_reason;
        if (status == RetirementStatus::Quarantined && !m_quarantine_logged) {
            m_quarantine_logged = true;
            log_status = true;
        } else if (status == RetirementStatus::MissingMarker && !m_missing_marker_logged) {
            m_missing_marker_logged = true;
            log_status = true;
        }
        if (log_status) {
            ++m_retirement_log_count;
        }
    }

    if (release) {
        release_generation();
        if (status == RetirementStatus::DeviceRemoved) {
            spdlog::warn("[RE4XeSS][Output] old output generation released after confirmed device removal");
        } else {
            spdlog::info("[RE4XeSS][Output] old handoff generation retired; bridge writer and downstream consumer proofs are complete");
        }
    } else if (status == RetirementStatus::MissingMarker || status == RetirementStatus::Quarantined) {
        if (log_status) {
            spdlog::warn("[RE4XeSS][Output] {}{}",
                retirement_status_message(status),
                status_reason.empty() ? std::string{} : ": " + status_reason);
        }
    }
    return status;
}

bool RE4XeSSOutputHandoff::prepare(
    sdk::renderer::layer::Overlay* layer,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Resource* semantic_color,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t display_width,
    uint32_t display_height,
    uint64_t control_generation,
    bool bridge_writer_idle,
    bool bridge_device_removed,
    RE4XeSSD3D12::OutputBinding& output,
    std::string& error) {
    output = {};
    error.clear();
    if (layer == nullptr || device == nullptr || queue == nullptr || semantic_color == nullptr) {
        error = "The RE4 XeSS handoff is missing Overlay, device, DIRECT queue, or semantic Color";
        return false;
    }

    auto* current_template = layer->get_main_target_state().get();
    if (current_template == nullptr) {
        error = "Overlay main TargetState is unavailable for XeSS output handoff";
        return false;
    }
    const auto color_description = semantic_color->GetDesc();
    Signature requested_signature{
        reinterpret_cast<uintptr_t>(current_template),
        reinterpret_cast<uintptr_t>(semantic_color),
        reinterpret_cast<uintptr_t>(device),
        reinterpret_cast<uintptr_t>(queue),
        control_generation,
        display_width,
        display_height,
        color_description.Format,
    };

    bool existing_generation_usable{};
    bool waiting_for_marker{};
    bool log_marker_block{};
    uint64_t waiting_marker_frame{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        waiting_for_marker = m_marker_pending || m_missing_marker;
        existing_generation_usable = !m_retirement_requested && !m_hard_quarantined &&
            !m_bridge_writer_uncertain && !m_device_removed && !waiting_for_marker;
        if (waiting_for_marker) {
            m_missing_marker = true;
            m_retirement_status = RetirementStatus::MissingMarker;
            if (!m_marker_wait_logged && m_delayed_marker_log_count < 8) {
                m_marker_wait_logged = true;
                ++m_delayed_marker_log_count;
                log_marker_block = true;
                waiting_marker_frame = m_installed_frame;
            }
        }
    }
    if (m_handoff_state != nullptr && m_signature == requested_signature && waiting_for_marker) {
        error = "Previous RE4 XeSS handoff frame is waiting for its downstream post-Present retirement marker";
        if (log_marker_block) {
            spdlog::warn("[RE4XeSS][Output] refusing handoff reuse while frame={} awaits post-Present settlement; no new XeSS output submit/install",
                static_cast<unsigned long long>(waiting_marker_frame));
        }
        return false;
    }
    if (m_handoff_state != nullptr && m_signature == requested_signature && existing_generation_usable) {
        output.resource = m_resource_pin.Get();
        output.before_state = m_expected_state;
        output.after_state = OUTPUT_READ_STATE;
        if (output.resource == nullptr) {
            error = "The active RE4 XeSS output handoff lost its native resource pin";
            return false;
        }
        return true;
    }

    if (m_handoff_state != nullptr) {
        request_retirement("handoff template/device/queue/mode/display generation changed");
        const auto status = poll_retirement(bridge_writer_idle, bridge_device_removed);
        if (status != RetirementStatus::Ready && status != RetirementStatus::DeviceRemoved &&
            status != RetirementStatus::NoGeneration) {
            error = "Previous RE4 XeSS output generation is not reusable: " + retirement_status_message(status);
            const auto state = snapshot();
            if (!state.failure_reason.empty()) {
                error += "; " + state.failure_reason;
            }
            return false;
        }
    }

    if (!validate_and_clone(
            layer,
            device,
            queue,
            semantic_color,
            render_width,
            render_height,
            display_width,
            display_height,
            error)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    const auto fence_result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(fence_result)) {
        error = "CreateFence(downstream retirement) failed with HRESULT " +
            std::to_string(static_cast<uint32_t>(fence_result));
        m_handoff_state.reset();
        m_resource_pin.Reset();
        return false;
    }
    m_resource_pin->SetName(L"RE4XeSS Handoff Output");

    m_signature = requested_signature;
    m_expected_state = D3D12_RESOURCE_STATE_COMMON;
    m_owner_thread_id = GetCurrentThreadId();
    m_next_retirement_value = 1;
    m_last_signaled_retirement_value = 0;
    m_last_completed_retirement_value = 0;
    m_installed_frame = 0;
    m_retirement_log_count = 0;
    m_restore_log_count = 0;
    m_delayed_marker_log_count = 0;
    m_installed = false;
    m_installed_overlay = nullptr;
    m_saved_original_state.reset();
    m_retirement_requested = false;
    m_downstream_use_seen = false;
    m_marker_pending = false;
    m_missing_marker = false;
    m_marker_wait_logged = false;
    m_marker_recovery_pending = false;
    m_hard_quarantined = false;
    m_bridge_writer_uncertain = false;
    m_missing_marker_logged = false;
    m_quarantine_logged = false;
    m_device_removed = false;
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_device = device;
        m_queue = queue;
        m_retirement_fence = std::move(fence);
        m_failure_reason.clear();
        m_retirement_status = RetirementStatus::Active;
        m_has_generation_snapshot.store(true, std::memory_order_release);
        m_installed_snapshot.store(false, std::memory_order_release);
    }
    spdlog::info("[RE4XeSS][Output] cloned display handoff target state=0x{:x} resource=0x{:x} render={}x{} display={}x{} firstState=COMMON",
        reinterpret_cast<uintptr_t>(m_handoff_state.get()),
        reinterpret_cast<uintptr_t>(m_resource_pin.Get()),
        render_width,
        render_height,
        display_width,
        display_height);

    output.resource = m_resource_pin.Get();
    output.before_state = m_expected_state;
    output.after_state = OUTPUT_READ_STATE;
    return true;
}

bool RE4XeSSOutputHandoff::validate_and_clone(
    sdk::renderer::layer::Overlay* layer,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12Resource* semantic_color,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t display_width,
    uint32_t display_height,
    std::string& error) {
    if (render_width == 0 || render_height == 0 || display_width == 0 || display_height == 0) {
        error = "The RE4 XeSS handoff extents must be non-zero";
        return false;
    }
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "The RE4 XeSS output handoff requires the active DIRECT queue";
        return false;
    }

    auto& current_state = layer->get_main_target_state();
    if (current_state == nullptr || current_state->get_rtv_count() != 1 ||
        current_state->get_native_resource_d3d12() != semantic_color) {
        error = "Overlay main must be the single-RTV semantic Color TargetState before handoff";
        return false;
    }
    const auto source_description = semantic_color->GetDesc();
    if (!valid_texture_extent(source_description, render_width, render_height, DXGI_FORMAT_R11G11B10_FLOAT) ||
        (source_description.Flags & REQUIRED_OUTPUT_FLAGS) != REQUIRED_OUTPUT_FLAGS) {
        error = "The semantic Color source must be render-resolution R11G11B10_FLOAT with render-target and UAV flags";
        return false;
    }

    std::vector<std::array<uint32_t, 2>> dimensions{
        std::array<uint32_t, 2>{ display_width, display_height }
    };
    auto cloned_state = current_state->clone(dimensions);
    if (cloned_state == nullptr || cloned_state.get() == current_state.get() ||
        cloned_state->get_rtv_count() != 1) {
        error = "TargetState::clone did not produce a distinct single-RTV handoff target";
        return false;
    }
    auto* resource = cloned_state->get_native_resource_d3d12();
    if (resource == nullptr || resource == semantic_color) {
        error = "The cloned handoff TargetState has no distinct native D3D12 resource";
        return false;
    }
    const auto output_description = resource->GetDesc();
    if (!valid_texture_extent(output_description, display_width, display_height, DXGI_FORMAT_R11G11B10_FLOAT) ||
        (output_description.Flags & REQUIRED_OUTPUT_FLAGS) != REQUIRED_OUTPUT_FLAGS) {
        error = "The cloned handoff resource is not a display-resolution R11G11B10_FLOAT render-target/UAV Texture2D";
        return false;
    }
    const auto& rect = cloned_state->get_desc().rect;
    if (rect.left != 0.0f || rect.top != 0.0f ||
        rect.right != static_cast<float>(display_width) || rect.bottom != static_cast<float>(display_height)) {
        error = "The cloned handoff TargetState rect does not match the display extent";
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    const auto support_result = device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    constexpr auto required_support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    if (FAILED(support_result) || (support.Support1 & required_support1) != required_support1 ||
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        error = "The cloned handoff resource format lacks the typed UAV support required by XeSS";
        return false;
    }

    m_handoff_state = std::move(cloned_state);
    m_resource_pin = resource;
    return true;
}

void RE4XeSSOutputHandoff::note_submission_succeeded() {
    m_expected_state = OUTPUT_READ_STATE;
}

bool RE4XeSSOutputHandoff::install(
    sdk::renderer::layer::Overlay* layer,
    uint64_t frame_id,
    std::string& error) {
    error.clear();
    if (layer == nullptr || m_handoff_state == nullptr || m_resource_pin == nullptr) {
        error = "The RE4 XeSS output generation is unavailable for Overlay installation";
        return false;
    }
    std::unique_lock lock{ m_retirement_mutex };
    if (m_retirement_requested || m_hard_quarantined || m_bridge_writer_uncertain || m_device_removed) {
        error = "The RE4 XeSS output generation is retiring or quarantined";
        return false;
    }
    const auto previous_frame = m_installed_frame;
    const bool log_marker_recovery = m_marker_recovery_pending && m_delayed_marker_log_count < 8;
    if (m_marker_recovery_pending) {
        m_marker_recovery_pending = false;
        if (log_marker_recovery) {
            ++m_delayed_marker_log_count;
        }
    }
    auto& main_state = layer->get_main_target_state();
    if (main_state.get() != reinterpret_cast<sdk::renderer::TargetState*>(m_signature.template_state) ||
        m_expected_state != OUTPUT_READ_STATE) {
        error = "Overlay main or output state changed before the post-submit handoff install";
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = error;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] output handoff quarantined: {}", error);
        return false;
    }

    m_saved_original_state = main_state;
    main_state = m_handoff_state;
    m_installed_overlay = layer;
    m_installed = true;
    m_installed_frame = frame_id;
    m_downstream_use_seen = true;
    m_marker_pending = true;
    m_missing_marker = false;
    m_installed_snapshot.store(true, std::memory_order_release);
    m_retirement_status = RetirementStatus::Active;
    m_failure_reason.clear();
    lock.unlock();
    bool log_install{};
    {
        std::lock_guard log_lock{ m_retirement_mutex };
        log_install = REFrameworkConfig::get()->is_debug_log_enabled() && m_retirement_log_count < 32;
        if (log_install) {
            ++m_retirement_log_count;
        }
    }
    if (log_install) {
        spdlog::info("[RE4XeSS][Output] installed handoff frame={} template=0x{:x} state=0x{:x} resource=0x{:x} extent={}x{} state=0xC0",
            static_cast<unsigned long long>(frame_id),
            m_signature.template_state,
            reinterpret_cast<uintptr_t>(m_handoff_state.get()),
            reinterpret_cast<uintptr_t>(m_resource_pin.Get()),
            m_signature.display_width,
            m_signature.display_height);
    }
    if (log_marker_recovery) {
        spdlog::info("[RE4XeSS][Output] resumed handoff after frame={} marker settlement; installed frame={}",
            static_cast<unsigned long long>(previous_frame),
            static_cast<unsigned long long>(frame_id));
    }
    return true;
}

void RE4XeSSOutputHandoff::quarantine(std::string_view reason, bool bridge_writer_uncertain) noexcept {
    std::string failure;
    {
        std::lock_guard lock{ m_retirement_mutex };
        m_bridge_writer_uncertain = bridge_writer_uncertain;
        m_hard_quarantined = !bridge_writer_uncertain;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        if (!reason.empty()) {
            m_failure_reason.assign(reason);
        }
        failure = m_failure_reason;
    }
    spdlog::error("[RE4XeSS][Failure] output handoff quarantined: {}", failure);
}

void RE4XeSSOutputHandoff::on_post_present(
    ID3D12Device* active_device,
    ID3D12CommandQueue* active_queue) noexcept {
    std::unique_lock lock{ m_retirement_mutex };
    if (!m_has_generation_snapshot.load(std::memory_order_acquire) || !m_marker_pending ||
        m_hard_quarantined || m_retirement_fence == nullptr || m_queue == nullptr || m_device == nullptr) {
        return;
    }
    if (active_device != m_device.Get() || active_queue != m_queue.Get()) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "post-Present active device/queue does not match the handoff generation";
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] downstream retirement marker rejected: {}", failure);
        return;
    }
    if (m_next_retirement_value == 0 || m_next_retirement_value == std::numeric_limits<uint64_t>::max()) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "Downstream retirement fence value space is exhausted";
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] downstream retirement marker rejected: {}", failure);
        return;
    }

    const bool settled_missing_marker = m_missing_marker;
    const auto value = m_next_retirement_value++;
    const auto result = m_queue->Signal(m_retirement_fence.Get(), value);
    if (FAILED(result)) {
        m_hard_quarantined = true;
        m_retirement_requested = true;
        m_retirement_status = RetirementStatus::Quarantined;
        m_failure_reason = "post-Present downstream retirement Signal failed with HRESULT " +
            std::to_string(static_cast<uint32_t>(result));
        const auto failure = m_failure_reason;
        lock.unlock();
        spdlog::error("[RE4XeSS][Failure] {}", failure);
        return;
    }

    m_last_signaled_retirement_value = value;
    m_marker_pending = false;
    m_missing_marker = false;
    m_marker_wait_logged = false;
    if (settled_missing_marker) {
        m_marker_recovery_pending = true;
    }
    m_retirement_status = m_retirement_requested ? RetirementStatus::Draining : RetirementStatus::Active;
    const bool log_marker = m_retirement_log_count < 32;
    if (log_marker) {
        ++m_retirement_log_count;
    }
    const auto frame = m_installed_frame;
    const auto queue_identity = reinterpret_cast<uintptr_t>(m_queue.Get());
    const bool log_marker_settlement = settled_missing_marker && m_delayed_marker_log_count < 8;
    if (log_marker_settlement) {
        ++m_delayed_marker_log_count;
    }
    lock.unlock();
    if (log_marker) {
        spdlog::info("[RE4XeSS][Output] downstream retirement marker queued value={} frame={} queue=0x{:x}",
            static_cast<unsigned long long>(value),
            static_cast<unsigned long long>(frame),
            queue_identity);
    }
    if (log_marker_settlement) {
        spdlog::info("[RE4XeSS][Output] queued delayed-marker settlement for restored handoff frame={} value={}; no additional output reader was installed before this marker",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(value));
    }
}

RE4XeSSOutputHandoff::Snapshot RE4XeSSOutputHandoff::snapshot() const {
    std::lock_guard lock{ m_retirement_mutex };
    return {
        m_has_generation_snapshot.load(std::memory_order_acquire),
        m_installed_snapshot.load(std::memory_order_acquire),
        m_retirement_status,
        m_failure_reason,
    };
}

void RE4XeSSOutputHandoff::observe_overlay(sdk::renderer::layer::Overlay* layer) const {
    if (layer == nullptr || !REFrameworkConfig::get()->is_debug_log_enabled()) {
        return;
    }
    uintptr_t expected_state{};
    uintptr_t resource_identity{};
    uint64_t frame{};
    uint32_t width{};
    uint32_t height{};
    bool should_log{};
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (!m_installed || layer != m_installed_overlay || m_retirement_log_count >= 32) {
            return;
        }
        ++m_retirement_log_count;
        expected_state = reinterpret_cast<uintptr_t>(m_handoff_state.get());
        resource_identity = reinterpret_cast<uintptr_t>(m_resource_pin.Get());
        frame = m_installed_frame;
        width = m_signature.display_width;
        height = m_signature.display_height;
        should_log = true;
    }
    if (!should_log) {
        return;
    }
    const auto& main_state = layer->get_main_target_state();
    const bool still_installed = reinterpret_cast<uintptr_t>(main_state.get()) == expected_state;
    if (still_installed) {
        spdlog::info("[RE4XeSS][Output] post-Overlay observation confirms handoff remains installed frame={} state=0x{:x} resource=0x{:x} extent={}x{}",
            static_cast<unsigned long long>(frame), expected_state, resource_identity, width, height);
    } else {
        spdlog::error("[RE4XeSS][Failure] Overlay main TargetState changed before the post-Overlay observation frame={}",
            static_cast<unsigned long long>(frame));
    }
}

void RE4XeSSOutputHandoff::release_generation() noexcept {
    sdk::intrusive_ptr<sdk::renderer::TargetState> handoff_state;
    sdk::intrusive_ptr<sdk::renderer::TargetState> saved_original_state;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    {
        std::lock_guard lock{ m_retirement_mutex };
        if (m_installed || m_marker_pending || m_bridge_writer_uncertain ||
            (m_hard_quarantined && !m_device_removed)) {
            return;
        }
        m_has_generation_snapshot.store(false, std::memory_order_release);
        m_installed_snapshot.store(false, std::memory_order_release);
        m_retirement_status = RetirementStatus::NoGeneration;
        m_marker_pending = false;
        m_missing_marker = false;
        m_retirement_requested = false;
        m_failure_reason.clear();
        handoff_state = std::move(m_handoff_state);
        saved_original_state = std::move(m_saved_original_state);
        resource = std::move(m_resource_pin);
        device = std::move(m_device);
        queue = std::move(m_queue);
        fence = std::move(m_retirement_fence);
    }
    m_signature = {};
    m_expected_state = D3D12_RESOURCE_STATE_COMMON;
    m_owner_thread_id = 0;
    m_next_retirement_value = 1;
    m_last_signaled_retirement_value = 0;
    m_last_completed_retirement_value = 0;
    m_installed_frame = 0;
    m_installed_overlay = nullptr;
    m_downstream_use_seen = false;
    m_bridge_writer_uncertain = false;
    m_device_removed = false;
}

void RE4XeSSOutputHandoff::preserve_quarantined_generation() noexcept {
    std::lock_guard lock{ m_retirement_mutex };
    m_hard_quarantined = true;
    m_retirement_requested = true;
    m_retirement_status = RetirementStatus::Quarantined;
    auto* retained = new (std::nothrow) RetainedOutputGeneration{};
    if (retained != nullptr) {
        retained->handoff_state = std::move(m_handoff_state);
        retained->saved_original_state = std::move(m_saved_original_state);
        retained->resource = std::move(m_resource_pin);
        retained->device = std::move(m_device);
        retained->queue = std::move(m_queue);
        retained->fence = std::move(m_retirement_fence);
        return;
    }

    if (m_handoff_state != nullptr) {
        m_handoff_state->add_ref();
        m_handoff_state.reset();
    }
    if (m_saved_original_state != nullptr) {
        m_saved_original_state->add_ref();
        m_saved_original_state.reset();
    }
    (void)m_resource_pin.Detach();
    (void)m_device.Detach();
    (void)m_queue.Detach();
    (void)m_retirement_fence.Detach();
}

bool RE4XeSSOutputHandoff::confirmed_device_removal(bool bridge_device_removed) const noexcept {
    if (bridge_device_removed || m_device_removed) {
        return true;
    }
    return m_device != nullptr && FAILED(m_device->GetDeviceRemovedReason());
}
