#include "RE4TemporalProbe.hpp"

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
constexpr std::array<const char*, 8> SCENARIOS{
    "Static screen",
    "Camera pan right",
    "Camera pan left",
    "Camera pan up",
    "Camera pan down",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
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
}

void RE4TemporalProbe::reset_temporal_state() {
    m_temporal_budget.reset();
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
    if (ImGui::Checkbox("Enable jitter + sparse MV readback test (default off)", &enabled)) {
        reset_temporal_state();
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] deterministic jitter test {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        m_scenario.store(scenario, std::memory_order_relaxed);
    }

    ImGui::Text(
        "Jitter samples: %u / %u",
        m_temporal_budget.sample_count(),
        re4_temporal_probe::MAX_TEMPORAL_SAMPLES);
    ImGui::TextWrapped(
        "RE4-only diagnostic. For Camera pan right/left/up/down, samples 1-4 are warm-up and samples 5-20 "
        "read a 3x3 VelocityTarget grid. Pan continuously in the selected direction during capture. "
        "MV semantics are closed at W/2,-H/2. The probe now also logs primary Camera near/far and compares "
        "Camera/SceneInfo Z-projection terms against normal and inverted D3D depth formulas. Static screen is "
        "sufficient for the next depth-convention capture; directional MV readback remains available for regression.");

    if (ImGui::Button("Reset directional capture")) {
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

    const auto scenario_index = m_scenario.load(std::memory_order_relaxed);
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

    // Capture directional camera motion only after four warm-up frames.
    const auto scenario = m_scenario.load(std::memory_order_relaxed);
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
}

void RE4TemporalProbe::on_device_reset() {
    m_velocity_copy_ready = false;
    m_velocity_copy = nullptr;
    m_mv_readback_failed = false;
    release_mv_readback_resources();
    reset_temporal_state();
}
