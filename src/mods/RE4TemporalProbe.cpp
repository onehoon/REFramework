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

#include "REFramework.hpp"

namespace {
constexpr std::array<const char*, 5> SCENARIOS{
    "Static screen",
    "Camera pan",
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

    m_history_valid.fill(false);
    m_expected_frame = 0;
    m_expected_sample = 0;
    m_expected_matrix_jitter_x = 0.0f;
    m_expected_matrix_jitter_y = 0.0f;
    m_expected_p20.fill(0.0f);
    m_expected_p21.fill(0.0f);
    m_last_mv_readback_sample = 0;
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
    if (!m_velocity_copy_ready || m_velocity_copy == nullptr) {
        return;
    }

    const auto finish = [this]() {
        m_velocity_copy_ready = false;
        m_velocity_copy = nullptr;
    };

    if (!ensure_mv_readback_resources()) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} failed to initialize readback resources",
            m_velocity_copy_sample,
            m_velocity_copy_frame);
        finish();
        return;
    }

    auto* container = m_velocity_copy->get_d3d12_resource_container();
    auto* source = container != nullptr ? container->get_native_resource() : nullptr;
    if (source == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} diagnostic clone has no native resource",
            m_velocity_copy_sample,
            m_velocity_copy_frame);
        finish();
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
        finish();
        return;
    }

    auto& hook = g_framework->get_d3d12_hook();
    auto* queue = hook != nullptr ? hook->get_command_queue() : nullptr;
    if (queue == nullptr) {
        spdlog::error("[RE4TemporalProbe] mvReadback missing D3D12 command queue");
        finish();
        return;
    }

    if (FAILED(m_mv_command_allocator->Reset()) ||
        FAILED(m_mv_command_list->Reset(m_mv_command_allocator.Get(), nullptr))) {
        spdlog::error("[RE4TemporalProbe] mvReadback failed to reset command objects");
        finish();
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
        finish();
        return;
    }

    ID3D12CommandList* lists[]{m_mv_command_list.Get()};
    queue->ExecuteCommandLists(1, lists);

    const auto fence_value = ++m_mv_fence_value;
    if (FAILED(queue->Signal(m_mv_fence.Get(), fence_value)) ||
        FAILED(m_mv_fence->SetEventOnCompletion(fence_value, m_mv_fence_event))) {
        spdlog::error("[RE4TemporalProbe] mvReadback failed to arm completion fence");
        finish();
        return;
    }

    const auto wait_result = WaitForSingleObject(m_mv_fence_event, 2000);
    if (wait_result != WAIT_OBJECT_0) {
        spdlog::error(
            "[RE4TemporalProbe] mvReadback sample={} frame={} fence wait failed result={}",
            m_velocity_copy_sample,
            m_velocity_copy_frame,
            wait_result);
        finish();
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
        "render={}x{} points={} cloneResource={:p} assumedCloneState=COPY_DEST",
        m_velocity_copy_sample,
        scenario_name(m_velocity_copy_scenario),
        m_velocity_copy_frame,
        phase,
        m_velocity_copy_width,
        m_velocity_copy_height,
        re4_temporal_probe::MV_SAMPLE_POINT_COUNT,
        static_cast<void*>(source));

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

        spdlog::info(
            "[RE4TemporalProbe] mvReadback sample={} scenario='{}' frame={} phase={} "
            "point={} coord={}x{} raw={{r={},g={},b={},a={}}} "
            "snorm={{r={:.9f},g={:.9f},b={:.9f},a={:.9f}}}",
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
            re4_temporal_probe::decode_snorm16(raw[0]),
            re4_temporal_probe::decode_snorm16(raw[1]),
            re4_temporal_probe::decode_snorm16(raw[2]),
            re4_temporal_probe::decode_snorm16(raw[3]));
    }

    D3D12_RANGE written_range{0, 0};
    m_mv_readback_buffer->Unmap(0, &written_range);

    m_last_mv_readback_sample = m_velocity_copy_sample;
    finish();
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable deterministic jitter injection test (default off)", &enabled)) {
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
        "RE4-only diagnostic. Injects a 4-phase +/-0.5 pixel jitter pattern. "
        "For Static screen only, the first 8 samples snapshot VelocityTarget into a disposable clone "
        "and read back a 3x3 interior texel grid. The game's original VelocityTarget is never barriered.");

    if (ImGui::Button("Reset jitter test")) {
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

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    const auto pixel_jitter = re4_temporal_probe::jitter_pixels_for_sample(sample);
    const auto matrix_jitter = re4_temporal_probe::projection_jitter_from_pixels(
        pixel_jitter,
        static_cast<uint32_t>(velocity.width),
        velocity.height);

    const auto camera_frame = m_camera_frame.load(std::memory_order_relaxed);
    const auto camera_same_frame = camera_frame == *frame;
    const auto camera_p20 = m_camera_p20.load(std::memory_order_relaxed);
    const auto camera_p21 = m_camera_p21.load(std::memory_order_relaxed);

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

        previous_projection[2][0] += matrix_jitter.x;
        previous_projection[2][1] += matrix_jitter.y;
        const auto history_projection_p20 = previous_projection[2][0];
        const auto history_projection_p21 = previous_projection[2][1];

        info->old_view_projection_matrix = previous_projection * previous_view;

        m_previous_projection[i] = current_projection;
        m_previous_view[i] = current_view;
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


bool RE4TemporalProbe::on_pre_overlay_layer_draw(
    sdk::renderer::layer::Overlay* layer,
    void* render_context) {
    if (!re4_temporal_probe::should_process_scene(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer) ||
        render_context == nullptr) {
        return true;
    }

    // First readback stage is intentionally Static-screen only and limited to
    // eight frames (two complete diagnostic jitter cycles).
    const auto scenario = m_scenario.load(std::memory_order_relaxed);
    if (scenario != 0 ||
        m_expected_sample == 0 ||
        m_expected_sample > re4_temporal_probe::MAX_MV_READBACK_SAMPLES ||
        m_expected_sample <= m_last_mv_readback_sample ||
        m_velocity_copy_ready) {
        return true;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value() || *frame != m_expected_frame) {
        return true;
    }

    auto* scene = static_cast<sdk::renderer::layer::Scene*>(layer->get_parent());
    if (scene == nullptr ||
        !scene->is_fully_rendered() ||
        scene->get_camera() != sdk::get_primary_camera()) {
        return true;
    }

    auto* motion_state = scene->get_motion_vectors_state();
    auto* rtv = motion_state != nullptr ? motion_state->get_rtv(0) : nullptr;
    if (rtv == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget RTV0 is null",
            m_expected_sample,
            *frame);
        return true;
    }

    auto& source_texture_ptr = rtv->get_texture_d3d12();
    auto* source_texture = source_texture_ptr.get();
    if (source_texture == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget texture is null",
            m_expected_sample,
            *frame);
        return true;
    }

    auto* source_container = source_texture->get_d3d12_resource_container();
    auto* source_resource =
        source_container != nullptr ? source_container->get_native_resource() : nullptr;
    if (source_resource == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} VelocityTarget native resource is null",
            m_expected_sample,
            *frame);
        return true;
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
        return true;
    }

    m_velocity_copy = source_texture->clone();
    if (m_velocity_copy == nullptr) {
        spdlog::error(
            "[RE4TemporalProbe] mvSnapshot sample={} frame={} failed to clone VelocityTarget",
            m_expected_sample,
            *frame);
        return true;
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
        return true;
    }

    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    context->copy_texture(m_velocity_copy.get(), source_texture);

    m_velocity_copy_sample = m_expected_sample;
    m_velocity_copy_frame = *frame;
    m_velocity_copy_width = static_cast<uint32_t>(desc.Width);
    m_velocity_copy_height = desc.Height;
    m_velocity_copy_scenario = scenario;
    m_velocity_copy_ready = true;

    spdlog::info(
        "[RE4TemporalProbe] mvSnapshotQueued sample={} scenario='{}' frame={} "
        "sourceResource={:p} cloneResource={:p} extent={}x{} format={}",
        m_velocity_copy_sample,
        scenario_name(scenario),
        m_velocity_copy_frame,
        static_cast<void*>(source_resource),
        static_cast<void*>(copy_resource),
        m_velocity_copy_width,
        m_velocity_copy_height,
        static_cast<uint32_t>(desc.Format));

    return true;
}

void RE4TemporalProbe::on_present() {
    // Drain any snapshot that was already queued by the engine even if the user
    // toggled the diagnostic off before Present.
    if (m_velocity_copy_ready) {
        perform_mv_readback();
    }
}

void RE4TemporalProbe::on_device_reset() {
    m_velocity_copy_ready = false;
    m_velocity_copy = nullptr;
    release_mv_readback_resources();
    reset_temporal_state();
}
