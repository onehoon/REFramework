#include "RE4TemporalProbe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <d3d12.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>
#include <sdk/Math.hpp>
#include <sdk/SceneManager.hpp>
#include <sdk/RETypeDB.hpp>

#include "REFramework.hpp"

namespace {
constexpr std::array<const char*, 12> SCENARIOS{
    "Static screen",
    "Camera pan right",
    "Camera pan left",
    "Camera pan up",
    "Camera pan down",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
    "Reset/history transition",
    "Load/fade state",
    "D3D12 execution ordering",
    "D3D12 resource states",
};

constexpr std::array<const char*, 6> LOAD_STATE_SINGLETONS{
    "share.FadeManager",
    "share.SaveDataManager",
    "share.MainModeManager",
    "chainsaw.SceneLoadZoneManager",
    "chainsaw.GameSituationManager",
    "share.SceneActivateMediator",
};

constexpr std::array<const char*, 6> SCENE_INFO_NAMES{
    "main",
    "depthDistortion",
    "filter",
    "jitterDisable",
    "jitterDisablePost",
    "zPrepass",
};

struct ResourceShape {
    uint64_t width{};
    uint32_t height{};
    uint32_t format{};
    uint32_t flags{};
};

const char* scenario_name(int index) {
    if (index < 0 || index >= (int)SCENARIOS.size()) {
        return "Unknown";
    }

    return SCENARIOS[index];
}

ResourceShape resource_shape(ID3D12Resource* resource) {
    if (resource == nullptr) {
        return {};
    }

    const auto desc = resource->GetDesc();
    return {
        .width = desc.Width,
        .height = desc.Height,
        .format = static_cast<uint32_t>(desc.Format),
        .flags = static_cast<uint32_t>(desc.Flags),
    };
}

struct RotationReprojectionResult {
    bool valid{};
    re4_temporal_probe::MotionPixelCandidate pixels{};
};

RotationReprojectionResult rotation_only_reprojection_delta(
    const Matrix4x4f& current_projection,
    const Matrix4x4f& current_view,
    const Matrix4x4f& previous_projection,
    const Matrix4x4f& previous_view,
    re4_temporal_probe::MVSamplePoint point,
    uint32_t width,
    uint32_t height) {
    if (width == 0 || height == 0) {
        return {};
    }

    const auto current_ndc = re4_temporal_probe::pixel_to_ndc(
        {static_cast<float>(point.x), static_cast<float>(point.y)},
        width,
        height);

    // Use an arbitrary finite clip-space depth to recover the current view ray.
    // The witness then transforms only the direction (w=0), intentionally
    // excluding camera translation so it remains independent of scene depth.
    const glm::vec4 current_clip{current_ndc.x, current_ndc.y, 0.5f, 1.0f};
    auto current_view_position = glm::inverse(current_projection) * current_clip;
    if (!std::isfinite(current_view_position.w) || std::abs(current_view_position.w) < 0.000001f) {
        return {};
    }

    current_view_position /= current_view_position.w;
    const glm::vec4 current_view_direction{
        current_view_position.x,
        current_view_position.y,
        current_view_position.z,
        0.0f,
    };

    const auto world_direction = glm::inverse(current_view) * current_view_direction;
    const auto previous_view_direction = previous_view * world_direction;
    const auto previous_clip = previous_projection * previous_view_direction;
    if (!std::isfinite(previous_clip.w) || std::abs(previous_clip.w) < 0.000001f) {
        return {};
    }

    const auto previous_ndc_x = previous_clip.x / previous_clip.w;
    const auto previous_ndc_y = previous_clip.y / previous_clip.w;
    if (!std::isfinite(previous_ndc_x) || !std::isfinite(previous_ndc_y)) {
        return {};
    }

    const auto previous_pixel = re4_temporal_probe::ndc_to_pixel(
        {previous_ndc_x, previous_ndc_y},
        width,
        height);

    return {
        .valid = true,
        .pixels = {
            previous_pixel.x - static_cast<float>(point.x),
            previous_pixel.y - static_cast<float>(point.y),
        },
    };
}

std::array<sdk::renderer::SceneInfo*, 6> scene_infos(sdk::renderer::layer::Scene* layer) {
    return {
        layer->get_scene_info(),
        layer->get_depth_distortion_scene_info(),
        layer->get_filter_scene_info(),
        layer->get_jitter_disable_scene_info(),
        layer->get_jitter_disable_post_scene_info(),
        layer->get_z_prepass_scene_info(),
    };
}

struct CameraPoseDelta {
    bool valid{};
    float translation{};
    float rotation_degrees{};
};

CameraPoseDelta camera_pose_delta(
    const Matrix4x4f& previous_view,
    const Matrix4x4f& current_view) {
    const auto previous_world = glm::inverse(previous_view);
    const auto current_world = glm::inverse(current_view);

    const glm::vec3 previous_position{previous_world[3]};
    const glm::vec3 current_position{current_world[3]};
    const auto translation = glm::length(current_position - previous_position);

    auto previous_forward = glm::vec3(previous_world[2]);
    auto current_forward = glm::vec3(current_world[2]);
    const auto previous_length = glm::length(previous_forward);
    const auto current_length = glm::length(current_forward);

    if (!std::isfinite(translation) ||
        previous_length < 0.000001f ||
        current_length < 0.000001f) {
        return {};
    }

    previous_forward /= previous_length;
    current_forward /= current_length;

    const auto dot = std::clamp(glm::dot(previous_forward, current_forward), -1.0f, 1.0f);
    const auto rotation_radians = std::acos(dot);
    if (!std::isfinite(rotation_radians)) {
        return {};
    }

    return {
        .valid = true,
        .translation = translation,
        .rotation_degrees = rotation_radians * 57.29577951308232f,
    };
}

struct MatrixError {
    bool valid{};
    float max_abs{};
    float sum_abs{};
};

MatrixError matrix_error(const Matrix4x4f& lhs, const Matrix4x4f& rhs) {
    MatrixError result{.valid = true};

    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row) {
            const auto a = lhs[column][row];
            const auto b = rhs[column][row];
            if (!std::isfinite(a) || !std::isfinite(b)) {
                return {};
            }

            const auto error = std::abs(a - b);
            result.max_abs = std::max(result.max_abs, error);
            result.sum_abs += error;
        }
    }

    return result;
}

const char* history_relation(
    bool reference_valid,
    const MatrixError& old_vs_previous,
    const MatrixError& old_vs_current) {
    if (!reference_valid) {
        return "unavailable";
    }

    if (!old_vs_previous.valid || !old_vs_current.valid) {
        return "invalid";
    }

    if (old_vs_previous.max_abs < old_vs_current.max_abs) {
        return "closerToPrevious";
    }

    if (old_vs_current.max_abs < old_vs_previous.max_abs) {
        return "closerToCurrent";
    }

    return "equal";
}

uint32_t integral_field_width(sdk::RETypeDefinition* type) {
    if (type == nullptr) {
        return 0;
    }

    auto* value_type = type;
    if (type->is_enum()) {
        value_type = type->get_underlying_type();
        if (value_type == nullptr) {
            return 0;
        }
    }

    const auto name = value_type->get_full_name();
    if (name == "System.Boolean" ||
        name == "System.SByte" ||
        name == "System.Byte") {
        return 1;
    }

    if (name == "System.Char" ||
        name == "System.Int16" ||
        name == "System.UInt16") {
        return 2;
    }

    if (name == "System.Int32" ||
        name == "System.UInt32") {
        return 4;
    }

    if (name == "System.Int64" ||
        name == "System.UInt64") {
        return 8;
    }

    return 0;
}

struct IntegralFieldValue {
    bool valid{};
    uint32_t width{};
    uint64_t bits{};
};

IntegralFieldValue read_integral_field(
    sdk::REField* field,
    REManagedObject* object) {
    if (field == nullptr || field->is_literal()) {
        return {};
    }

    auto* field_type = field->get_type();
    const auto width = integral_field_width(field_type);
    if (width == 0) {
        return {};
    }

    auto* data = field->get_data_raw(object);
    if (data == nullptr || IsBadReadPtr(data, width)) {
        return {};
    }

    uint64_t bits = 0;
    std::memcpy(&bits, data, width);
    return {
        .valid = true,
        .width = width,
        .bits = bits,
    };
}

REManagedObject* get_load_state_singleton(std::string_view type_name) {
    using Getter = REManagedObject* (*)();
    static std::unordered_map<std::string, Getter> getters{};

    const auto key = std::string{type_name};
    if (const auto it = getters.find(key); it != getters.end()) {
        return it->second != nullptr ? it->second() : nullptr;
    }

    auto* type = sdk::find_type_definition(type_name);
    auto getter = type != nullptr
        ? reinterpret_cast<Getter>(sdk::find_native_method(type, "get_Instance"))
        : nullptr;

    getters.emplace(key, getter);
    return getter != nullptr ? getter() : nullptr;
}
}

void RE4TemporalProbe::reset_temporal_state() {
    m_temporal_budget.reset();
    m_reset_watch_budget.reset();
    m_load_state_budget.reset();
    m_execution_order_budget.reset();
    m_resource_state_budget.reset();
    m_execution_boundary_frame.store(0, std::memory_order_relaxed);
    m_execution_boundary_sample.store(0, std::memory_order_relaxed);
    m_execution_submit_count.store(0, std::memory_order_relaxed);
    m_execution_boundary_submit_base.store(0, std::memory_order_relaxed);
    m_resource_boundary_frame.store(0, std::memory_order_relaxed);
    m_resource_boundary_sample.store(0, std::memory_order_relaxed);
    m_resource_submit_count.store(0, std::memory_order_relaxed);
    m_resource_boundary_submit_base.store(0, std::memory_order_relaxed);
    m_resource_event_sequence.store(0, std::memory_order_relaxed);
    m_resource_color.store(0, std::memory_order_relaxed);
    m_resource_depth.store(0, std::memory_order_relaxed);
    m_resource_velocity.store(0, std::memory_order_relaxed);
    {
        std::scoped_lock lock{m_resource_state_mutex};
        m_resource_active_by_thread.clear();
    }
    m_reset_witness_valid = false;
    m_reset_previous_frame = 0;
    m_reset_previous_scene = 0;
    m_reset_previous_scene_info = 0;
    m_reset_previous_camera = 0;
    m_reset_previous_depth = 0;
    m_reset_previous_velocity = 0;
    m_reset_previous_color = 0;
    m_reset_previous_width = 0;
    m_reset_previous_height = 0;
    m_reset_previous_view = {};
    m_reset_previous_view_projection = {};
    m_reset_previous_view_projection_valid = false;
    m_load_state_witness_valid = false;
    m_load_state_previous_frame = 0;
    m_load_state_previous_view = {};
    m_load_state_objects.clear();
    m_load_state_values.clear();
    m_load_state_schema_keys.clear();
    m_camera_ptr.store(0, std::memory_order_relaxed);
    m_camera_frame.store(0, std::memory_order_relaxed);
    m_camera_p20.store(0.0f, std::memory_order_relaxed);
    m_camera_p21.store(0.0f, std::memory_order_relaxed);
    m_camera_p22.store(0.0f, std::memory_order_relaxed);
    m_camera_p23.store(0.0f, std::memory_order_relaxed);
    m_camera_p32.store(0.0f, std::memory_order_relaxed);
    m_camera_p33.store(0.0f, std::memory_order_relaxed);
    m_camera_near.store(0.0f, std::memory_order_relaxed);
    m_camera_far.store(0.0f, std::memory_order_relaxed);
    m_camera_clip_valid.store(false, std::memory_order_relaxed);

    m_history_valid.fill(false);
    m_previous_scene_frame.fill(0);
    m_expected_frame = 0;
    m_expected_sample = 0;
    m_expected_matrix_jitter_x = 0.0f;
    m_expected_matrix_jitter_y = 0.0f;
    m_expected_p20.fill(0.0f);
    m_expected_p21.fill(0.0f);
    m_expected_rotation_reprojection.fill({});
    m_expected_rotation_reprojection_previous_frame = 0;
    m_expected_rotation_reprojection_frame = 0;
    m_expected_rotation_reprojection_valid = false;
    m_velocity_copy_rotation_reprojection.fill({});
    m_velocity_copy_rotation_reprojection_previous_frame = 0;
    m_velocity_copy_rotation_reprojection_valid = false;
}

bool RE4TemporalProbe::ensure_execution_queue_hook() {
    if (m_execution_queue_hook != nullptr && m_execution_queue_original != nullptr) {
        return true;
    }

    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    auto& d3d12 = g_framework->get_d3d12_hook();
    auto* queue = d3d12 != nullptr ? d3d12->get_command_queue() : nullptr;
    if (queue == nullptr) {
        spdlog::error("[RE4TemporalProbe] executionOrder missing D3D12 command queue");
        return false;
    }

    const auto desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        spdlog::error(
            "[RE4TemporalProbe] executionOrder expected DIRECT queue but got type={}",
            static_cast<uint32_t>(desc.Type));
        return false;
    }

    try {
        auto hook = std::make_unique<VtableHook>(Address{queue});
        auto** vtable = *reinterpret_cast<void***>(queue);
        if (vtable == nullptr || vtable[10] == nullptr) {
            spdlog::error("[RE4TemporalProbe] executionOrder queue vtable/ExecuteCommandLists missing");
            return false;
        }

        const auto original = reinterpret_cast<ExecuteCommandListsFn>(vtable[10]);
        if (!hook->hook_method(
                10,
                Address{reinterpret_cast<void*>(&RE4TemporalProbe::execute_command_lists_hook)})) {
            spdlog::error("[RE4TemporalProbe] executionOrder failed to hook ExecuteCommandLists[10]");
            return false;
        }

        m_execution_queue_original = original;
        m_execution_queue_hook = std::move(hook);
        s_execution_probe_instance = this;

        spdlog::info(
            "[RE4TemporalProbe] executionOrderHook queue={:p} type={} original={:p}",
            static_cast<void*>(queue),
            static_cast<uint32_t>(desc.Type),
            reinterpret_cast<void*>(original));
        return true;
    } catch (const std::exception& e) {
        spdlog::error("[RE4TemporalProbe] executionOrder hook exception={}", e.what());
    } catch (...) {
        spdlog::error("[RE4TemporalProbe] executionOrder hook unknown exception");
    }

    return false;
}

void RE4TemporalProbe::release_execution_queue_hook() {
    if (s_execution_probe_instance == this) {
        s_execution_probe_instance = nullptr;
    }

    m_execution_queue_hook.reset();
    m_execution_queue_original = nullptr;
    m_execution_boundary_frame.store(0, std::memory_order_relaxed);
    m_execution_boundary_sample.store(0, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE RE4TemporalProbe::execute_command_lists_hook(
    ID3D12CommandQueue* queue,
    UINT num_command_lists,
    ID3D12CommandList* const* command_lists) {
    auto* self = s_execution_probe_instance;
    auto original = self != nullptr ? self->m_execution_queue_original : nullptr;

    if (self != nullptr &&
        self->m_enabled.load(std::memory_order_relaxed) &&
        re4_temporal_probe::is_execution_order_scenario(
            self->m_scenario.load(std::memory_order_relaxed))) {
        const auto sample = self->m_execution_boundary_sample.load(std::memory_order_relaxed);
        const auto boundary_frame = self->m_execution_boundary_frame.load(std::memory_order_relaxed);

        if (sample != 0 && boundary_frame != 0) {
            const auto submit = self->m_execution_submit_count.fetch_add(
                1,
                std::memory_order_relaxed) + 1;
            const auto queue_desc = queue != nullptr ? queue->GetDesc() : D3D12_COMMAND_QUEUE_DESC{};

            spdlog::info(
                "[RE4TemporalProbe] executionSubmit submit={} sample={} boundaryFrame={} "
                "queue={:p} queueType={} numLists={} thread={}",
                submit,
                sample,
                boundary_frame,
                static_cast<void*>(queue),
                static_cast<uint32_t>(queue_desc.Type),
                num_command_lists,
                GetCurrentThreadId());

            for (UINT i = 0; i < num_command_lists; ++i) {
                auto* list = command_lists != nullptr ? command_lists[i] : nullptr;
                spdlog::info(
                    "[RE4TemporalProbe] executionList submit={} sample={} boundaryFrame={} "
                    "index={} list={:p} type={}",
                    submit,
                    sample,
                    boundary_frame,
                    i,
                    static_cast<void*>(list),
                    list != nullptr
                        ? static_cast<uint32_t>(list->GetType())
                        : static_cast<uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT));
            }
        }
    }

    if (original != nullptr) {
        original(queue, num_command_lists, command_lists);
    }
}

bool RE4TemporalProbe::ensure_mv_readback_resources() {
    if (m_mv_command_allocator != nullptr &&
        m_mv_command_list != nullptr &&
        m_mv_readback_buffer != nullptr &&
        m_mv_fence != nullptr &&
        m_mv_fence_event != nullptr) {
        return true;
    }

    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    auto& hook = g_framework->get_d3d12_hook();
    if (hook == nullptr || hook->get_device() == nullptr) {
        return false;
    }

    auto* device = hook->get_device();

    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_mv_command_allocator)))) {
        spdlog::error("[RE4TemporalProbe] failed to create MV readback command allocator");
        release_mv_readback_resources();
        return false;
    }

    if (FAILED(device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            m_mv_command_allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&m_mv_command_list)))) {
        spdlog::error("[RE4TemporalProbe] failed to create MV readback command list");
        release_mv_readback_resources();
        return false;
    }

    if (FAILED(m_mv_command_list->Close())) {
        spdlog::error("[RE4TemporalProbe] failed to close initial MV readback command list");
        release_mv_readback_resources();
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    heap_props.CreationNodeMask = 1;
    heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = re4_temporal_probe::MV_READBACK_BUFFER_SIZE;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_mv_readback_buffer)))) {
        spdlog::error("[RE4TemporalProbe] failed to create MV readback buffer");
        release_mv_readback_resources();
        return false;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_mv_fence)))) {
        spdlog::error("[RE4TemporalProbe] failed to create MV readback fence");
        release_mv_readback_resources();
        return false;
    }

    m_mv_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_mv_fence_event == nullptr) {
        spdlog::error("[RE4TemporalProbe] failed to create MV readback fence event");
        release_mv_readback_resources();
        return false;
    }

    m_mv_fence_value = 0;
    return true;
}

void RE4TemporalProbe::release_mv_readback_resources() {
    if (m_mv_fence_event != nullptr) {
        CloseHandle(m_mv_fence_event);
        m_mv_fence_event = nullptr;
    }

    m_mv_fence.Reset();
    m_mv_readback_buffer.Reset();
    m_mv_command_list.Reset();
    m_mv_command_allocator.Reset();
    m_mv_fence_value = 0;
}

void RE4TemporalProbe::perform_mv_readback() {
    if (!m_velocity_copy_ready || m_velocity_copy == nullptr || m_mv_readback_failed) {
        return;
    }

    const auto finish = [this]() {
        m_velocity_copy_ready = false;
        m_velocity_copy = nullptr;
    };

    const auto fail_hold = [this](const char* reason) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback fail-closed reason={}; "
            "probe disabled and diagnostic clone retained until device reset",
            reason);
        m_mv_readback_failed = true;
        m_enabled.store(false, std::memory_order_relaxed);
        m_velocity_copy_ready = false;
    };

    if (!ensure_mv_readback_resources()) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} failed to initialize readback resources",
            m_velocity_copy_sample,
            m_velocity_copy_frame);
        fail_hold("readback_resource_init");
        return;
    }

    auto* container = m_velocity_copy->get_d3d12_resource_container();
    auto* source = container != nullptr ? container->get_native_resource() : nullptr;
    if (source == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} diagnostic clone has no native resource",
            m_velocity_copy_sample,
            m_velocity_copy_frame);
        fail_hold("clone_native_resource");
        return;
    }

    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R16G16B16A16_SNORM ||
        desc.SampleDesc.Count != 1 ||
        desc.Width != m_velocity_copy_width ||
        desc.Height != m_velocity_copy_height) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} unexpected clone desc width={} height={} format={} samples={}",
            m_velocity_copy_sample,
            m_velocity_copy_frame,
            desc.Width,
            desc.Height,
            static_cast<uint32_t>(desc.Format),
            desc.SampleDesc.Count);
        fail_hold("clone_desc");
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto* queue = hook != nullptr ? hook->get_command_queue() : nullptr;
    if (queue == nullptr) {
        spdlog::error("[RE4TemporalProbe] mvReadback missing D3D12 command queue");
        fail_hold("command_queue");
        return;
    }

    if (FAILED(m_mv_command_allocator->Reset()) ||
        FAILED(m_mv_command_list->Reset(m_mv_command_allocator.Get(), nullptr))) {
        spdlog::error("[RE4TemporalProbe] mvReadback failed to reset command objects");
        fail_hold("command_reset");
        return;
    }

    // The diagnostic clone's only engine-side use is the immediately preceding
    // RenderContext::copy_texture destination. Keep this state assumption isolated
    // to the disposable clone; never place a diagnostic barrier on VelocityTarget.
    D3D12_RESOURCE_BARRIER to_copy_source{};
    to_copy_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy_source.Transition.pResource = source;
    to_copy_source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy_source.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_copy_source.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_mv_command_list->ResourceBarrier(1, &to_copy_source);

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = source;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = 0;

    for (uint32_t i = 0; i < re4_temporal_probe::MV_SAMPLE_POINT_COUNT; ++i) {
        const auto point = re4_temporal_probe::mv_sample_point(
            i,
            m_velocity_copy_width,
            m_velocity_copy_height);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_mv_readback_buffer.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset =
            i * re4_temporal_probe::MV_READBACK_POINT_STRIDE;
        destination.PlacedFootprint.Footprint.Format =
            DXGI_FORMAT_R16G16B16A16_SNORM;
        destination.PlacedFootprint.Footprint.Width = 1;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch =
            D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

        D3D12_BOX box{};
        box.left = point.x;
        box.top = point.y;
        box.front = 0;
        box.right = point.x + 1;
        box.bottom = point.y + 1;
        box.back = 1;

        m_mv_command_list->CopyTextureRegion(
            &destination,
            0,
            0,
            0,
            &source_location,
            &box);
    }

    if (FAILED(m_mv_command_list->Close())) {
        spdlog::error("[RE4TemporalProbe] mvReadback failed to close command list");
        fail_hold("command_close");
        return;
    }

    ID3D12CommandList* lists[]{m_mv_command_list.Get()};
    queue->ExecuteCommandLists(1, lists);

    const auto fence_value = ++m_mv_fence_value;
    const auto signal_result = queue->Signal(m_mv_fence.Get(), fence_value);
    const auto event_result = SUCCEEDED(signal_result)
        ? m_mv_fence->SetEventOnCompletion(fence_value, m_mv_fence_event)
        : signal_result;

    if (FAILED(signal_result) || FAILED(event_result)) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback failed to arm completion fence signal=0x{:08x} event=0x{:08x}; "
            "probe disabled and resources retained until device reset",
            static_cast<uint32_t>(signal_result),
            static_cast<uint32_t>(event_result));
        fail_hold("fence_arm");
        return;
    }

    const auto wait_result = WaitForSingleObject(m_mv_fence_event, 500);
    if (wait_result != WAIT_OBJECT_0) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} fence wait failed result={}; "
            "probe disabled and resources retained until device reset",
            m_velocity_copy_sample,
            m_velocity_copy_frame,
            wait_result);
        fail_hold("fence_wait");
        return;
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, re4_temporal_probe::MV_READBACK_BUFFER_SIZE};
    if (FAILED(m_mv_readback_buffer->Map(0, &read_range, &mapped)) || mapped == nullptr) {
        spdlog::error("[RE4TemporalProbe] mvReadback failed to map readback buffer");
        finish();
        return;
    }

    const auto phase =
        (m_velocity_copy_sample - 1) % re4_temporal_probe::JITTER_PHASE_COUNT;

    spdlog::info(
        "[RE4TemporalProbe] mvReadbackBegin sample={} scenario='{}' frame={} phase={} "
        "render={}x{} points={} cloneResource={:p} assumedCloneState=COPY_DEST "
        "rotationOnlyReprojectionValid={} rotationPair={{previousFrame={},currentFrame={}}}",
        m_velocity_copy_sample,
        scenario_name(m_velocity_copy_scenario),
        m_velocity_copy_frame,
        phase,
        m_velocity_copy_width,
        m_velocity_copy_height,
        re4_temporal_probe::MV_SAMPLE_POINT_COUNT,
        static_cast<void*>(source),
        m_velocity_copy_rotation_reprojection_valid,
        m_velocity_copy_rotation_reprojection_previous_frame,
        m_velocity_copy_frame);

    const auto* bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t i = 0; i < re4_temporal_probe::MV_SAMPLE_POINT_COUNT; ++i) {
        const auto point = re4_temporal_probe::mv_sample_point(
            i,
            m_velocity_copy_width,
            m_velocity_copy_height);

        std::array<int16_t, 4> raw{};
        std::memcpy(
            raw.data(),
            bytes + i * re4_temporal_probe::MV_READBACK_POINT_STRIDE,
            sizeof(raw));

        const auto r_snorm = re4_temporal_probe::decode_snorm16(raw[0]);
        const auto g_snorm = re4_temporal_probe::decode_snorm16(raw[1]);
        const auto b_snorm = re4_temporal_probe::decode_snorm16(raw[2]);
        const auto a_snorm = re4_temporal_probe::decode_snorm16(raw[3]);
        const auto pixel_candidate =
            re4_temporal_probe::historical_motion_pixel_candidate(
                r_snorm,
                g_snorm,
                m_velocity_copy_width,
                m_velocity_copy_height);

        const auto reprojection = m_velocity_copy_rotation_reprojection_valid
            ? m_velocity_copy_rotation_reprojection[i]
            : re4_temporal_probe::MotionPixelCandidate{};
        const re4_temporal_probe::MotionPixelCandidate residual{
            pixel_candidate.x - reprojection.x,
            pixel_candidate.y - reprojection.y,
        };

        spdlog::info(
            "[RE4TemporalProbe] mvReadback sample={} scenario='{}' frame={} phase={} "
            "point={} coord={}x{} raw={{r={},g={},b={},a={}}} "
            "snorm={{r={:.9f},g={:.9f},b={:.9f},a={:.9f}}} "
            "historicalPixelCandidate={{x={:.6f},y={:.6f}}} "
            "rotationOnlyReprojectionValid={} rotationPair={{previousFrame={},currentFrame={}}} "
            "rotationOnlyReprojection={{x={:.6f},y={:.6f}}} "
            "candidateMinusReprojection={{x={:.6f},y={:.6f}}}",
            m_velocity_copy_sample,
            scenario_name(m_velocity_copy_scenario),
            m_velocity_copy_frame,
            phase,
            i,
            point.x,
            point.y,
            raw[0],
            raw[1],
            raw[2],
            raw[3],
            r_snorm,
            g_snorm,
            b_snorm,
            a_snorm,
            pixel_candidate.x,
            pixel_candidate.y,
            m_velocity_copy_rotation_reprojection_valid,
            m_velocity_copy_rotation_reprojection_previous_frame,
            m_velocity_copy_frame,
            reprojection.x,
            reprojection.y,
            residual.x,
            residual.y);
    }

    D3D12_RANGE written_range{0, 0};
    m_mv_readback_buffer->Unmap(0, &written_range);

    finish();
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    if (m_mv_readback_failed) {
        m_enabled.store(false, std::memory_order_relaxed);
        ImGui::TextWrapped(
            "Sparse MV readback failed closed. Restart or a D3D12 device reset is required before retrying.");
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable RE4 temporal diagnostic (default off)", &enabled)) {
        reset_temporal_state();
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] temporal diagnostic {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        reset_temporal_state();
        m_scenario.store(scenario, std::memory_order_relaxed);
        spdlog::info(
            "[RE4TemporalProbe] capture scenario changed to '{}'; capture state reset",
            scenario_name(scenario));
    }

    if (re4_temporal_probe::is_reset_history_scenario(scenario)) {
        ImGui::Text(
            "Reset/history samples: %u / %u",
            m_reset_watch_budget.sample_count(),
            re4_temporal_probe::RESET_WATCH_MAX_SAMPLES);
    } else if (re4_temporal_probe::is_load_state_scenario(scenario)) {
        ImGui::Text(
            "Load-state samples: %u / %u",
            m_load_state_budget.sample_count(),
            re4_temporal_probe::LOAD_STATE_MAX_SAMPLES);
    } else if (re4_temporal_probe::is_execution_order_scenario(scenario)) {
        ImGui::Text(
            "Execution-order samples: %u / %u",
            m_execution_order_budget.sample_count(),
            re4_temporal_probe::EXECUTION_ORDER_MAX_SAMPLES);
    } else if (re4_temporal_probe::is_resource_state_scenario(scenario)) {
        ImGui::Text(
            "Resource-state samples: %u / %u",
            m_resource_state_budget.sample_count(),
            re4_temporal_probe::RESOURCE_STATE_MAX_SAMPLES);
    } else {
        ImGui::Text(
            "Jitter samples: %u / %u",
            m_temporal_budget.sample_count(),
            re4_temporal_probe::MAX_TEMPORAL_SAMPLES);
    }

    ImGui::TextWrapped(
        "RE4-only diagnostic. Capture 23 closed DIRECT queue/list provenance. For Gate H Capture 24, select "
        "D3D12 resource states. The probe dynamically hooks only DIRECT command lists observed on the active queue, "
        "tracks Reset/Close recording generations, logs ResourceBarrier only for the current Color/Depth/Velocity "
        "resources, and correlates the pre-Overlay worker thread with its active list/generation. It remains "
        "observe-only and issues no GPU work, barriers, copies, or XeSS calls.");

    if (ImGui::Button("Reset capture")) {
        reset_temporal_state();
    }
}

void RE4TemporalProbe::on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) {
    if (!re4_temporal_probe::should_process_camera_projection(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            camera,
            result)) {
        return;
    }

    auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || camera != reinterpret_cast<REManagedObject*>(primary_camera)) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value()) {
        return;
    }

    m_camera_ptr.store(reinterpret_cast<uintptr_t>(camera), std::memory_order_relaxed);
    m_camera_frame.store(*frame, std::memory_order_relaxed);
    m_camera_p20.store((*result)[2][0], std::memory_order_relaxed);
    m_camera_p21.store((*result)[2][1], std::memory_order_relaxed);
    m_camera_p22.store((*result)[2][2], std::memory_order_relaxed);
    m_camera_p23.store((*result)[2][3], std::memory_order_relaxed);
    m_camera_p32.store((*result)[3][2], std::memory_order_relaxed);
    m_camera_p33.store((*result)[3][3], std::memory_order_relaxed);

    static auto via_camera = sdk::find_type_definition("via.Camera");
    static auto get_near_clip_plane_method =
        via_camera != nullptr ? via_camera->get_method("get_NearClipPlane") : nullptr;
    static auto get_far_clip_plane_method =
        via_camera != nullptr ? via_camera->get_method("get_FarClipPlane") : nullptr;

    bool clip_valid = false;
    float near_plane = 0.0f;
    float far_plane = 0.0f;

    if (get_near_clip_plane_method != nullptr && get_far_clip_plane_method != nullptr) {
        near_plane = get_near_clip_plane_method->call<float>(sdk::get_thread_context(), camera);
        far_plane = get_far_clip_plane_method->call<float>(sdk::get_thread_context(), camera);
        clip_valid =
            std::isfinite(near_plane) &&
            std::isfinite(far_plane) &&
            re4_temporal_probe::valid_clip_planes(near_plane, far_plane);
    }

    m_camera_near.store(near_plane, std::memory_order_relaxed);
    m_camera_far.store(far_plane, std::memory_order_relaxed);
    m_camera_clip_valid.store(clip_valid, std::memory_order_relaxed);
}

void RE4TemporalProbe::on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return;
    }

    if (!layer->is_fully_rendered()) {
        return;
    }

    auto* primary_camera = sdk::get_primary_camera();
    auto* scene_camera = layer->get_camera();
    if (primary_camera == nullptr || scene_camera != primary_camera) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value()) {
        return;
    }

    const auto scenario_index = m_scenario.load(std::memory_order_relaxed);
    if (re4_temporal_probe::is_execution_order_scenario(scenario_index) ||
        re4_temporal_probe::is_resource_state_scenario(scenario_index)) {
        ensure_execution_queue_hook();
        return;
    }

    if (re4_temporal_probe::is_load_state_scenario(scenario_index)) {
        const auto sample = m_load_state_budget.reserve_frame(
            *frame,
            re4_temporal_probe::LOAD_STATE_MAX_SAMPLES);
        if (sample == 0) {
            return;
        }

        auto* scene_info = layer->get_scene_info();
        const auto first = !m_load_state_witness_valid;
        const auto frame_gap =
            !first &&
            m_load_state_previous_frame != 0 &&
            *frame != m_load_state_previous_frame + 1;

        CameraPoseDelta pose_delta{};
        if (!first && scene_info != nullptr) {
            pose_delta = camera_pose_delta(
                m_load_state_previous_view,
                scene_info->view_matrix);
        }

        uint32_t manager_count = 0;
        uint32_t field_count = 0;
        uint32_t changed_field_count = 0;
        uint32_t object_change_count = 0;

        for (const auto* manager_name : LOAD_STATE_SINGLETONS) {
            REManagedObject* object = nullptr;
            try {
                object = get_load_state_singleton(manager_name);
            } catch (...) {
                object = nullptr;
            }

            const auto current_object = reinterpret_cast<uintptr_t>(object);
            const auto object_key = std::string{manager_name};
            const auto previous_object = m_load_state_objects.find(object_key);
            const auto object_changed =
                previous_object != m_load_state_objects.end() &&
                previous_object->second != current_object;

            if (previous_object == m_load_state_objects.end() || object_changed) {
                spdlog::info(
                    "[RE4TemporalProbe] loadStateObject sample={} frame={} manager={} "
                    "previous={:p} current={:p} changed={}",
                    sample,
                    *frame,
                    manager_name,
                    previous_object != m_load_state_objects.end()
                        ? reinterpret_cast<void*>(previous_object->second)
                        : nullptr,
                    static_cast<void*>(object),
                    object_changed);
                if (object_changed) {
                    ++object_change_count;
                }
            }
            m_load_state_objects[object_key] = current_object;

            if (object == nullptr) {
                continue;
            }

            ++manager_count;
            auto* type = object->get_type_definition();
            for (auto* current_type = type;
                 current_type != nullptr;
                 current_type = current_type->get_parent_type()) {
                for (auto* field : current_type->get_fields()) {
                    try {
                        if (field == nullptr || field->get_name() == nullptr) {
                            continue;
                        }

                        const auto value = read_integral_field(field, object);
                        if (!value.valid) {
                            continue;
                        }

                        auto* field_type = field->get_type();
                        auto* declaring_type = field->get_declaring_type();
                        const auto field_type_name =
                            field_type != nullptr ? field_type->get_full_name() : std::string{"<null>"};
                        const auto declaring_type_name =
                            declaring_type != nullptr ? declaring_type->get_full_name() : std::string{"<null>"};

                        std::string key{manager_name};
                        key += "|";
                        key += declaring_type_name;
                        key += "|";
                        key += field->get_name();

                        ++field_count;

                        if (m_load_state_schema_keys.insert(key).second) {
                            spdlog::info(
                                "[RE4TemporalProbe] loadStateSchema manager={} field={} "
                                "declaringType={} fieldType={} offset={} static={} enum={} width={} "
                                "baselineBits=0x{:016x}",
                                manager_name,
                                field->get_name(),
                                declaring_type_name,
                                field_type_name,
                                field->get_offset_from_base(),
                                field->is_static(),
                                field_type != nullptr && field_type->is_enum(),
                                value.width,
                                value.bits);
                        }

                        const auto previous_value = m_load_state_values.find(key);
                        if (previous_value != m_load_state_values.end() &&
                            previous_value->second != value.bits) {
                            ++changed_field_count;
                            spdlog::info(
                                "[RE4TemporalProbe] loadStateChange sample={} frame={} manager={} "
                                "field={} declaringType={} fieldType={} oldBits=0x{:016x} "
                                "newBits=0x{:016x} translationDelta={:.9f} rotationDeltaDegrees={:.6f}",
                                sample,
                                *frame,
                                manager_name,
                                field->get_name(),
                                declaring_type_name,
                                field_type_name,
                                previous_value->second,
                                value.bits,
                                pose_delta.translation,
                                pose_delta.rotation_degrees);
                        }

                        m_load_state_values[key] = value.bits;
                    } catch (...) {
                        continue;
                    }
                }
            }
        }

        spdlog::info(
            "[RE4TemporalProbe] loadStateWitness sample={} frame={} first={} frameGap={} "
            "managerCount={} fieldCount={} changedFields={} objectChanges={} "
            "poseValid={} translationDelta={:.9f} rotationDeltaDegrees={:.6f}",
            sample,
            *frame,
            first,
            frame_gap,
            manager_count,
            field_count,
            changed_field_count,
            object_change_count,
            pose_delta.valid,
            pose_delta.translation,
            pose_delta.rotation_degrees);

        m_load_state_witness_valid = true;
        m_load_state_previous_frame = *frame;
        if (scene_info != nullptr) {
            m_load_state_previous_view = scene_info->view_matrix;
        }

        return;
    }

    if (re4_temporal_probe::is_reset_history_scenario(scenario_index)) {
        const auto watch_sample = m_reset_watch_budget.reserve_frame(
            *frame,
            re4_temporal_probe::RESET_WATCH_MAX_SAMPLES);
        if (watch_sample == 0) {
            return;
        }

        auto* scene_info = layer->get_scene_info();
        auto* depth_resource = layer->get_depth_stencil_d3d12();
        auto* velocity_resource = layer->get_motion_vectors_d3d12();
        auto* color_resource = layer->get_post_main_target_d3d12();
        const auto velocity = resource_shape(velocity_resource);

        const auto current_scene = reinterpret_cast<uintptr_t>(layer);
        const auto current_scene_info = reinterpret_cast<uintptr_t>(scene_info);
        const auto current_camera = reinterpret_cast<uintptr_t>(scene_camera);
        const auto current_depth = reinterpret_cast<uintptr_t>(depth_resource);
        const auto current_velocity = reinterpret_cast<uintptr_t>(velocity_resource);
        const auto current_color = reinterpret_cast<uintptr_t>(color_resource);
        const auto current_width = static_cast<uint32_t>(velocity.width);
        const auto current_height = velocity.height;

        const auto first = !m_reset_witness_valid;
        const auto frame_gap =
            !first && m_reset_previous_frame != 0 && *frame != m_reset_previous_frame + 1;
        const auto scene_changed = !first && current_scene != m_reset_previous_scene;
        const auto scene_info_changed = !first && current_scene_info != m_reset_previous_scene_info;
        const auto camera_changed = !first && current_camera != m_reset_previous_camera;
        const auto depth_changed = !first && current_depth != m_reset_previous_depth;
        const auto velocity_changed = !first && current_velocity != m_reset_previous_velocity;
        const auto color_changed = !first && current_color != m_reset_previous_color;
        const auto render_size_changed =
            !first &&
            (current_width != m_reset_previous_width || current_height != m_reset_previous_height);

        CameraPoseDelta pose_delta{};
        if (!first && scene_info != nullptr) {
            pose_delta = camera_pose_delta(m_reset_previous_view, scene_info->view_matrix);
        }

        const auto history_reference_valid =
            !first &&
            !frame_gap &&
            scene_info != nullptr &&
            m_reset_previous_view_projection_valid;
        MatrixError old_vs_previous{};
        MatrixError old_vs_current{};
        if (history_reference_valid) {
            old_vs_previous = matrix_error(
                scene_info->old_view_projection_matrix,
                m_reset_previous_view_projection);
            old_vs_current = matrix_error(
                scene_info->old_view_projection_matrix,
                scene_info->view_projection_matrix);
        }

        spdlog::info(
            "[RE4TemporalProbe] resetWitness sample={} frame={} first={} frameGap={} "
            "sceneChanged={} sceneInfoChanged={} cameraChanged={} depthChanged={} "
            "velocityChanged={} colorChanged={} renderSizeChanged={} render={}x{} "
            "poseValid={} translationDelta={:.9f} rotationDeltaDegrees={:.6f} "
            "ptrs={{scene={:p},sceneInfo={:p},camera={:p},depth={:p},velocity={:p},color={:p}}}",
            watch_sample,
            *frame,
            first,
            frame_gap,
            scene_changed,
            scene_info_changed,
            camera_changed,
            depth_changed,
            velocity_changed,
            color_changed,
            render_size_changed,
            current_width,
            current_height,
            pose_delta.valid,
            pose_delta.translation,
            pose_delta.rotation_degrees,
            static_cast<void*>(layer),
            static_cast<void*>(scene_info),
            static_cast<void*>(scene_camera),
            static_cast<void*>(depth_resource),
            static_cast<void*>(velocity_resource),
            static_cast<void*>(color_resource));

        spdlog::info(
            "[RE4TemporalProbe] historyWitness sample={} frame={} previousFrame={} "
            "referenceValid={} oldVsPrevious={{valid={},maxAbs={:.9f},sumAbs={:.9f}}} "
            "oldVsCurrent={{valid={},maxAbs={:.9f},sumAbs={:.9f}}} relation={} "
            "translationDelta={:.9f} rotationDeltaDegrees={:.6f}",
            watch_sample,
            *frame,
            m_reset_previous_frame,
            history_reference_valid,
            old_vs_previous.valid,
            old_vs_previous.max_abs,
            old_vs_previous.sum_abs,
            old_vs_current.valid,
            old_vs_current.max_abs,
            old_vs_current.sum_abs,
            history_relation(history_reference_valid, old_vs_previous, old_vs_current),
            pose_delta.translation,
            pose_delta.rotation_degrees);

        m_reset_witness_valid = true;
        m_reset_previous_frame = *frame;
        m_reset_previous_scene = current_scene;
        m_reset_previous_scene_info = current_scene_info;
        m_reset_previous_camera = current_camera;
        m_reset_previous_depth = current_depth;
        m_reset_previous_velocity = current_velocity;
        m_reset_previous_color = current_color;
        m_reset_previous_width = current_width;
        m_reset_previous_height = current_height;
        if (scene_info != nullptr) {
            m_reset_previous_view = scene_info->view_matrix;
            m_reset_previous_view_projection = scene_info->view_projection_matrix;
            m_reset_previous_view_projection_valid = true;
        } else {
            m_reset_previous_view_projection_valid = false;
        }

        return;
    }

    const auto sample = m_temporal_budget.reserve_frame(*frame);
    if (sample == 0) {
        return;
    }

    auto* velocity_resource = layer->get_motion_vectors_d3d12();
    const auto velocity = resource_shape(velocity_resource);
    if (velocity.width == 0 || velocity.height == 0) {
        spdlog::error(
            "[RE4TemporalProbe] jitterSample={} scenario='{}' frame={} missing render extent from VelocityTarget",
            sample,
            scenario_name(m_scenario.load(std::memory_order_relaxed)),
            *frame);
        return;
    }

    auto infos = scene_infos(layer);
    if (infos[0] == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] jitterSample={} scenario='{}' frame={} main SceneInfo is null",
            sample,
            scenario_name(m_scenario.load(std::memory_order_relaxed)),
            *frame);
        return;
    }

    const auto scenario = scenario_name(scenario_index);
    const auto pixel_jitter = re4_temporal_probe::jitter_pixels_for_sample(sample);
    const auto matrix_jitter = re4_temporal_probe::projection_jitter_from_pixels(
        pixel_jitter,
        static_cast<uint32_t>(velocity.width),
        velocity.height);

    const auto camera_frame = m_camera_frame.load(std::memory_order_relaxed);
    const auto camera_same_frame = camera_frame == *frame;
    const auto camera_p20 = m_camera_p20.load(std::memory_order_relaxed);
    const auto camera_p21 = m_camera_p21.load(std::memory_order_relaxed);
    const auto camera_p22 = m_camera_p22.load(std::memory_order_relaxed);
    const auto camera_p23 = m_camera_p23.load(std::memory_order_relaxed);
    const auto camera_p32 = m_camera_p32.load(std::memory_order_relaxed);
    const auto camera_p33 = m_camera_p33.load(std::memory_order_relaxed);
    const auto camera_near = m_camera_near.load(std::memory_order_relaxed);
    const auto camera_far = m_camera_far.load(std::memory_order_relaxed);
    const auto camera_clip_valid = m_camera_clip_valid.load(std::memory_order_relaxed);

    spdlog::info(
        "[RE4TemporalProbe] jitterFrame sample={} scenario='{}' frame={} phase={} "
        "render={}x{} pixel={{x={:.6f},y={:.6f}}} matrix={{x={:.9f},y={:.9f}}} "
        "cameraFrame={} cameraSameFrame={} cameraXY={{p20={:.9f},p21={:.9f}}} "
        "velocityResource={:p} velocityFormat={} velocityFlags=0x{:x}",
        sample,
        scenario,
        *frame,
        (sample - 1) % re4_temporal_probe::JITTER_PHASE_COUNT,
        velocity.width,
        velocity.height,
        pixel_jitter.x,
        pixel_jitter.y,
        matrix_jitter.x,
        matrix_jitter.y,
        camera_frame,
        camera_same_frame,
        camera_p20,
        camera_p21,
        static_cast<void*>(velocity_resource),
        velocity.format,
        velocity.flags);

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            spdlog::info(
                "[RE4TemporalProbe] jitterVariant sample={} scenario='{}' frame={} name={} sceneInfo=null",
                sample,
                scenario,
                *frame,
                SCENE_INFO_NAMES[i]);
            continue;
        }

        const auto current_projection = info->projection_matrix;
        const auto current_view = info->view_matrix;
        const auto before_p20 = current_projection[2][0];
        const auto before_p21 = current_projection[2][1];

        auto previous_projection = m_history_valid[i] ? m_previous_projection[i] : current_projection;
        const auto previous_view = m_history_valid[i] ? m_previous_view[i] : current_view;
        const auto history_was_valid = m_history_valid[i];
        const auto previous_scene_frame = m_previous_scene_frame[i];

        if (i == 0) {
            const auto normal_depth =
                re4_temporal_probe::normal_depth_terms(camera_near, camera_far);
            const auto inverted_depth =
                re4_temporal_probe::inverted_depth_terms(camera_near, camera_far);
            const re4_temporal_probe::PerspectiveDepthTerms camera_depth{
                camera_p22,
                camera_p32,
            };
            const re4_temporal_probe::PerspectiveDepthTerms scene_depth{
                current_projection[2][2],
                current_projection[3][2],
            };

            const auto camera_normal_error = camera_clip_valid
                ? re4_temporal_probe::depth_terms_error(camera_depth, normal_depth)
                : 0.0f;
            const auto camera_inverted_error = camera_clip_valid
                ? re4_temporal_probe::depth_terms_error(camera_depth, inverted_depth)
                : 0.0f;
            const auto scene_normal_error = camera_clip_valid
                ? re4_temporal_probe::depth_terms_error(scene_depth, normal_depth)
                : 0.0f;
            const auto scene_inverted_error = camera_clip_valid
                ? re4_temporal_probe::depth_terms_error(scene_depth, inverted_depth)
                : 0.0f;

            const auto scene_depth_inference =
                !camera_clip_valid ? "unknown"
                : scene_inverted_error < scene_normal_error ? "inverted"
                : scene_normal_error < scene_inverted_error ? "normal"
                : "ambiguous";

            const auto projection_y = current_projection[1][1];
            const auto vertical_fov_radians =
                std::isfinite(projection_y) && std::abs(projection_y) > 0.000001f
                ? 2.0f * std::atan(1.0f / projection_y)
                : 0.0f;

            spdlog::info(
                "[RE4TemporalProbe] depthProjection sample={} scenario='{}' frame={} "
                "cameraFrame={} cameraSameFrame={} clipValid={} near={:.9f} far={:.9f} "
                "camera={{p22={:.9f},p23={:.9f},p32={:.9f},p33={:.9f}}} "
                "scene={{p22={:.9f},p23={:.9f},p32={:.9f},p33={:.9f},p11={:.9f}}} "
                "expectedNormal={{p22={:.9f},p32={:.9f}}} "
                "expectedInverted={{p22={:.9f},p32={:.9f}}} "
                "errors={{cameraNormal={:.9f},cameraInverted={:.9f},"
                "sceneNormal={:.9f},sceneInverted={:.9f}}} "
                "sceneDepthInference={} verticalFovRadians={:.9f}",
                sample,
                scenario,
                *frame,
                camera_frame,
                camera_same_frame,
                camera_clip_valid,
                camera_near,
                camera_far,
                camera_p22,
                camera_p23,
                camera_p32,
                camera_p33,
                current_projection[2][2],
                current_projection[2][3],
                current_projection[3][2],
                current_projection[3][3],
                projection_y,
                normal_depth.p22,
                normal_depth.p32,
                inverted_depth.p22,
                inverted_depth.p32,
                camera_normal_error,
                camera_inverted_error,
                scene_normal_error,
                scene_inverted_error,
                scene_depth_inference,
                vertical_fov_radians);

            m_expected_rotation_reprojection_previous_frame = previous_scene_frame;
            m_expected_rotation_reprojection_frame = *frame;
            m_expected_rotation_reprojection_valid = false;
            m_expected_rotation_reprojection.fill({});

            // Compute the witness for every directional sample, not only MV readback
            // samples. That lets post-processing align one MV frame against the
            // immediately prior/current/next camera-matrix pair by exact frame ID
            // without delaying or retaining the cloned VelocityTarget.
            if (history_was_valid &&
                previous_scene_frame != 0 &&
                re4_temporal_probe::is_directional_mv_scenario(scenario_index)) {
                bool witness_valid = true;

                for (uint32_t point_index = 0;
                     point_index < re4_temporal_probe::MV_SAMPLE_POINT_COUNT;
                     ++point_index) {
                    const auto point = re4_temporal_probe::mv_sample_point(
                        point_index,
                        static_cast<uint32_t>(velocity.width),
                        velocity.height);
                    const auto witness = rotation_only_reprojection_delta(
                        current_projection,
                        current_view,
                        m_previous_projection[i],
                        m_previous_view[i],
                        point,
                        static_cast<uint32_t>(velocity.width),
                        velocity.height);

                    witness_valid = witness_valid && witness.valid;
                    m_expected_rotation_reprojection[point_index] = witness.pixels;

                    spdlog::info(
                        "[RE4TemporalProbe] rotationReprojection sample={} scenario='{}' "
                        "previousFrame={} currentFrame={} point={} coord={}x{} valid={} "
                        "pixels={{x={:.6f},y={:.6f}}}",
                        sample,
                        scenario,
                        previous_scene_frame,
                        *frame,
                        point_index,
                        point.x,
                        point.y,
                        witness.valid,
                        witness.pixels.x,
                        witness.pixels.y);
                }

                m_expected_rotation_reprojection_valid = witness_valid;
            }
        }

        previous_projection[2][0] += matrix_jitter.x;
        previous_projection[2][1] += matrix_jitter.y;
        const auto history_projection_p20 = previous_projection[2][0];
        const auto history_projection_p21 = previous_projection[2][1];

        info->old_view_projection_matrix = previous_projection * previous_view;

        m_previous_projection[i] = current_projection;
        m_previous_view[i] = current_view;
        m_previous_scene_frame[i] = *frame;
        m_history_valid[i] = true;

        info->projection_matrix[2][0] += matrix_jitter.x;
        info->projection_matrix[2][1] += matrix_jitter.y;
        info->inverse_projection_matrix = glm::inverse(info->projection_matrix);
        info->view_projection_matrix = info->projection_matrix * info->view_matrix;
        info->inverse_view_projection_matrix = glm::inverse(info->view_projection_matrix);

        m_expected_p20[i] = info->projection_matrix[2][0];
        m_expected_p21[i] = info->projection_matrix[2][1];

        spdlog::info(
            "[RE4TemporalProbe] jitterVariant sample={} scenario='{}' frame={} name={} sceneInfo={:p} "
            "historyValid={} before={{p20={:.9f},p21={:.9f}}} "
            "historyProjection={{p20={:.9f},p21={:.9f}}} "
            "after={{p20={:.9f},p21={:.9f}}} oldVP={{m20={:.9f},m21={:.9f}}}",
            sample,
            scenario,
            *frame,
            SCENE_INFO_NAMES[i],
            static_cast<void*>(info),
            history_was_valid,
            before_p20,
            before_p21,
            history_projection_p20,
            history_projection_p21,
            info->projection_matrix[2][0],
            info->projection_matrix[2][1],
            info->old_view_projection_matrix[2][0],
            info->old_view_projection_matrix[2][1]);
    }

    m_expected_frame = *frame;
    m_expected_sample = sample;
    m_expected_matrix_jitter_x = matrix_jitter.x;
    m_expected_matrix_jitter_y = matrix_jitter.y;
}

bool RE4TemporalProbe::on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) {
    (void)render_context;

    const auto scenario = m_scenario.load(std::memory_order_relaxed);
    if (re4_temporal_probe::is_reset_history_scenario(scenario) ||
        re4_temporal_probe::is_load_state_scenario(scenario) ||
        re4_temporal_probe::is_execution_order_scenario(scenario) ||
        re4_temporal_probe::is_resource_state_scenario(scenario)) {
        return true;
    }

    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer)) {
        return true;
    }

    if (!layer->is_fully_rendered()) {
        return true;
    }

    auto* primary_camera = sdk::get_primary_camera();
    if (primary_camera == nullptr || layer->get_camera() != primary_camera) {
        return true;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value() || *frame != m_expected_frame || m_expected_sample == 0) {
        return true;
    }

    auto infos = scene_infos(layer);
    bool all_match = true;

    for (size_t i = 0; i < infos.size(); ++i) {
        auto* info = infos[i];
        if (info == nullptr) {
            continue;
        }

        const auto p20_matches = std::abs(info->projection_matrix[2][0] - m_expected_p20[i]) < 0.0000001f;
        const auto p21_matches = std::abs(info->projection_matrix[2][1] - m_expected_p21[i]) < 0.0000001f;
        all_match = all_match && p20_matches && p21_matches;
    }

    spdlog::info(
        "[RE4TemporalProbe] jitterDrawCheck sample={} scenario='{}' frame={} "
        "expectedMatrix={{x={:.9f},y={:.9f}}} allVariantsMatch={} "
        "mainActual={{p20={:.9f},p21={:.9f}}}",
        m_expected_sample,
        scenario_name(m_scenario.load(std::memory_order_relaxed)),
        *frame,
        m_expected_matrix_jitter_x,
        m_expected_matrix_jitter_y,
        all_match,
        infos[0] != nullptr ? infos[0]->projection_matrix[2][0] : 0.0f,
        infos[0] != nullptr ? infos[0]->projection_matrix[2][1] : 0.0f);

    return true;
}


void RE4TemporalProbe::on_overlay_layer_draw(
    sdk::renderer::layer::Overlay* layer,
    void* render_context) {
    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer) ||
        render_context == nullptr) {
        return;
    }

    const auto scenario = m_scenario.load(std::memory_order_relaxed);
    if (re4_temporal_probe::is_execution_order_scenario(scenario)) {
        auto* renderer = sdk::renderer::get_renderer();
        const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
        if (!frame.has_value()) {
            return;
        }

        const auto sample = m_execution_order_budget.reserve_frame(
            *frame,
            re4_temporal_probe::EXECUTION_ORDER_MAX_SAMPLES);
        if (sample == 0) {
            m_execution_boundary_sample.store(0, std::memory_order_relaxed);
            m_execution_boundary_frame.store(0, std::memory_order_relaxed);
            return;
        }

        if (!ensure_execution_queue_hook()) {
            spdlog::error(
                "[RE4TemporalProbe] executionBoundary sample={} frame={} queueHookUnavailable",
                sample,
                *frame);
            return;
        }

        auto* scene = static_cast<sdk::renderer::layer::Scene*>(layer->get_parent());
        auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
        auto* current_target = context->get_render_target();
        auto* current_target_resource =
            current_target != nullptr ? current_target->get_native_resource_d3d12() : nullptr;

        auto& overlay_main_ref = layer->get_main_target_state();
        auto* overlay_main = overlay_main_ref.get();
        auto* overlay_main_resource =
            overlay_main != nullptr ? overlay_main->get_native_resource_d3d12() : nullptr;

        auto* color = scene != nullptr ? scene->get_post_main_target_d3d12() : nullptr;
        auto* hdr = scene != nullptr ? scene->get_hdr_target_d3d12() : nullptr;
        auto* depth = scene != nullptr ? scene->get_depth_stencil_d3d12() : nullptr;
        auto* velocity = scene != nullptr ? scene->get_motion_vectors_d3d12() : nullptr;

        auto& d3d12 = g_framework->get_d3d12_hook();
        auto* queue = d3d12 != nullptr ? d3d12->get_command_queue() : nullptr;
        const auto queue_desc = queue != nullptr ? queue->GetDesc() : D3D12_COMMAND_QUEUE_DESC{};

        const auto submit_base = m_execution_submit_count.load(std::memory_order_relaxed);
        m_execution_boundary_submit_base.store(submit_base, std::memory_order_relaxed);
        m_execution_boundary_frame.store(*frame, std::memory_order_relaxed);
        m_execution_boundary_sample.store(sample, std::memory_order_release);

        spdlog::info(
            "[RE4TemporalProbe] executionBoundary sample={} frame={} stage=preOverlay "
            "thread={} renderContext={:p} protectFrame={} delayEnabled={} "
            "currentTarget={:p} currentResource={:p} overlayMain={:p} overlayMainResource={:p} "
            "color={:p} hdr={:p} depth={:p} velocity={:p} "
            "queue={:p} queueType={} submitBase={}",
            sample,
            *frame,
            GetCurrentThreadId(),
            render_context,
            context->get_protect_frame(),
            context->is_delay_enabled(),
            static_cast<void*>(current_target),
            static_cast<void*>(current_target_resource),
            static_cast<void*>(overlay_main),
            static_cast<void*>(overlay_main_resource),
            static_cast<void*>(color),
            static_cast<void*>(hdr),
            static_cast<void*>(depth),
            static_cast<void*>(velocity),
            static_cast<void*>(queue),
            static_cast<uint32_t>(queue_desc.Type),
            submit_base);
        return;
    }

    // Capture directional camera motion only after four warm-up frames.
    if (!re4_temporal_probe::should_readback_mv_sample(scenario, m_expected_sample) ||
        m_velocity_copy_ready) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value() || *frame != m_expected_frame) {
        return;
    }

    auto* scene = static_cast<sdk::renderer::layer::Scene*>(layer->get_parent());
    if (scene == nullptr ||
        !scene->is_fully_rendered() ||
        scene->get_camera() != sdk::get_primary_camera()) {
        return;
    }

    auto* motion_state = scene->get_motion_vectors_state();
    auto rtv = motion_state != nullptr ? motion_state->get_rtv(0) : nullptr;
    if (rtv == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget RTV0 is null",
            m_expected_sample,
            *frame);
        return;
    }

    auto& source_texture_ptr = rtv->get_texture_d3d12();
    auto* source_texture = source_texture_ptr.get();
    if (source_texture == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget texture is null",
            m_expected_sample,
            *frame);
        return;
    }

    auto* source_container = source_texture->get_d3d12_resource_container();
    auto* source_resource =
        source_container != nullptr ? source_container->get_native_resource() : nullptr;
    if (source_resource == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget native resource is null",
            m_expected_sample,
            *frame);
        return;
    }

    const auto desc = source_resource->GetDesc();
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_SNORM ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} unsupported VelocityTarget desc "
            "width={} height={} format={} samples={}",
            m_expected_sample,
            *frame,
            desc.Width,
            desc.Height,
            static_cast<uint32_t>(desc.Format),
            desc.SampleDesc.Count);
        return;
    }

    m_velocity_copy = source_texture->clone();
    if (m_velocity_copy == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} failed to clone VelocityTarget",
            m_expected_sample,
            *frame);
        return;
    }

    auto* copy_container = m_velocity_copy->get_d3d12_resource_container();
    auto* copy_resource =
        copy_container != nullptr ? copy_container->get_native_resource() : nullptr;
    if (copy_resource == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} cloned VelocityTarget has no native resource",
            m_expected_sample,
            *frame);
        m_velocity_copy = nullptr;
        return;
    }

    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    context->copy_texture(m_velocity_copy.get(), source_texture);

    m_velocity_copy_sample = m_expected_sample;
    m_velocity_copy_frame = *frame;
    m_velocity_copy_width = static_cast<uint32_t>(desc.Width);
    m_velocity_copy_height = desc.Height;
    m_velocity_copy_scenario = scenario;
    m_velocity_copy_rotation_reprojection_valid =
        m_expected_rotation_reprojection_valid &&
        m_expected_rotation_reprojection_frame == *frame;
    m_velocity_copy_rotation_reprojection_previous_frame =
        m_expected_rotation_reprojection_previous_frame;
    m_velocity_copy_rotation_reprojection = m_expected_rotation_reprojection;
    m_velocity_copy_ready = true;

    spdlog::info(
        "[RE4TemporalProbe] mvSnapshotQueued sample={} scenario='{}' frame={} "
        "sourceResource={:p} cloneResource={:p} extent={}x{} format={} "
        "rotationOnlyReprojectionValid={} rotationPair={{previousFrame={},currentFrame={}}}",
        m_velocity_copy_sample,
        scenario_name(scenario),
        m_velocity_copy_frame,
        static_cast<void*>(source_resource),
        static_cast<void*>(copy_resource),
        m_velocity_copy_width,
        m_velocity_copy_height,
        static_cast<uint32_t>(desc.Format),
        m_velocity_copy_rotation_reprojection_valid,
        m_velocity_copy_rotation_reprojection_previous_frame,
        m_velocity_copy_frame);

}

void RE4TemporalProbe::on_present() {
    // Drain any snapshot that was already queued by the engine even if the user
    // toggled the diagnostic off before Present. After a post-submit fence failure,
    // fail closed and retain GPU-referenced resources until device reset.
    if (m_velocity_copy_ready && !m_mv_readback_failed) {
        perform_mv_readback();
    }

    if (m_enabled.load(std::memory_order_relaxed) &&
        re4_temporal_probe::is_execution_order_scenario(
            m_scenario.load(std::memory_order_relaxed))) {
        const auto sample = m_execution_boundary_sample.load(std::memory_order_acquire);
        const auto boundary_frame = m_execution_boundary_frame.load(std::memory_order_relaxed);
        if (sample != 0 && boundary_frame != 0) {
            const auto submit_count = m_execution_submit_count.load(std::memory_order_relaxed);
            const auto submit_base = m_execution_boundary_submit_base.load(std::memory_order_relaxed);

            spdlog::info(
                "[RE4TemporalProbe] executionPresent sample={} boundaryFrame={} "
                "submitsSinceBoundary={} totalObservedSubmits={} thread={}",
                sample,
                boundary_frame,
                submit_count >= submit_base ? submit_count - submit_base : 0,
                submit_count,
                GetCurrentThreadId());

            m_execution_boundary_sample.store(0, std::memory_order_release);
            m_execution_boundary_frame.store(0, std::memory_order_relaxed);
        }
    }
}

void RE4TemporalProbe::on_device_reset() {
    m_velocity_copy_ready = false;
    m_velocity_copy = nullptr;
    m_mv_readback_failed = false;
    release_mv_readback_resources();
    release_execution_queue_hook();
    reset_temporal_state();
}
