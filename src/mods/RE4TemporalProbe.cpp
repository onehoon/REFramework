#include "RE4TemporalProbe.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <spdlog/spdlog.h>

#include <sdk/GameIdentity.hpp>

namespace {
constexpr uint32_t MAX_SAMPLES = re4_temporal_probe::MAX_SAMPLES;

constexpr std::array<const char*, 5> SCENARIOS{
    "Static screen",
    "Camera pan",
    "Character motion",
    "HUD/menu on",
    "HUD/menu off",
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

bool same_extent(const ResourceShape& a, const ResourceShape& b) {
    return a.width != 0 && a.height != 0 && a.width == b.width && a.height == b.height;
}
}

void RE4TemporalProbe::on_draw_ui() {
    if (!sdk::GameIdentity::get().is_re4() || !ImGui::CollapsingHeader("RE4 Temporal Probe")) {
        return;
    }

    bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Enable 1920x1080 render-size test (default off)", &enabled)) {
        m_enabled.store(enabled, std::memory_order_relaxed);
        spdlog::info("[RE4TemporalProbe] capture {}", enabled ? "enabled" : "disabled");
    }

    int scenario = m_scenario.load(std::memory_order_relaxed);
    if (ImGui::Combo("Capture scenario", &scenario, SCENARIOS.data(), (int)SCENARIOS.size())) {
        m_scenario.store(scenario, std::memory_order_relaxed);
    }

    ImGui::Text("Overlay callbacks: %u", m_size_sample_budget.callback_count());
    ImGui::Text("Size samples: %u / %u", m_size_sample_budget.sample_count(), MAX_SAMPLES);
    ImGui::TextWrapped(
        "RE4-only temporary split test. When enabled, SceneView.get_Size is overridden to 1920x1080; "
        "swapchain/output are not resized. The probe checks whether Color/Depth/Velocity follow the overridden extent.");

    if (ImGui::Button("Reset capture budget")) {
        m_size_sample_budget.reset();
        m_latest_scene_view.store(0, std::memory_order_relaxed);
        m_latest_view_frame.store(0, std::memory_order_relaxed);
        m_latest_view_width.store(0, std::memory_order_relaxed);
        m_latest_view_height.store(0, std::memory_order_relaxed);
        m_latest_original_view_width.store(0, std::memory_order_relaxed);
        m_latest_original_view_height.store(0, std::memory_order_relaxed);
        m_size_pair_sample.store(0, std::memory_order_relaxed);
        m_size_pair_frame.store(0, std::memory_order_relaxed);
    }
}

void RE4TemporalProbe::on_view_get_size(REManagedObject* scene_view, float* result) {
    if (!re4_temporal_probe::should_process_view_size(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            scene_view,
            result)) {
        return;
    }

    if (!std::isfinite(result[0]) || !std::isfinite(result[1]) || result[0] <= 0.0f || result[1] <= 0.0f) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;

    const auto original_width = result[0];
    const auto original_height = result[1];

    result[0] = static_cast<float>(re4_temporal_probe::TEST_RENDER_WIDTH);
    result[1] = static_cast<float>(re4_temporal_probe::TEST_RENDER_HEIGHT);

    m_latest_scene_view.store(reinterpret_cast<uintptr_t>(scene_view), std::memory_order_relaxed);
    m_latest_view_frame.store(frame.value_or(0), std::memory_order_relaxed);
    m_latest_view_width.store(re4_temporal_probe::TEST_RENDER_WIDTH, std::memory_order_relaxed);
    m_latest_view_height.store(re4_temporal_probe::TEST_RENDER_HEIGHT, std::memory_order_relaxed);
    m_latest_original_view_width.store(static_cast<uint32_t>(original_width + 0.5f), std::memory_order_relaxed);
    m_latest_original_view_height.store(static_cast<uint32_t>(original_height + 0.5f), std::memory_order_relaxed);
}

bool RE4TemporalProbe::on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    if (!re4_temporal_probe::should_process_overlay(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer,
            render_context)) {
        return true;
    }

    if (!m_size_sample_budget.should_sample_callback()) {
        return true;
    }

    const auto sample = m_size_sample_budget.reserve_sample();
    if (sample == 0) {
        return true;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));

    auto* scene = reinterpret_cast<sdk::renderer::layer::Scene*>(layer->get_parent());
    auto* main_target = layer->get_main_target_state().get();
    auto* color_resource = main_target != nullptr ? main_target->get_native_resource_d3d12() : nullptr;
    auto* post_main_resource = scene != nullptr ? scene->get_post_main_target_d3d12() : nullptr;
    auto* hdr_resource = scene != nullptr ? scene->get_hdr_target_d3d12() : nullptr;
    auto* depth_resource = scene != nullptr ? scene->get_depth_stencil_d3d12() : nullptr;
    auto* velocity_resource = scene != nullptr ? scene->get_motion_vectors_d3d12() : nullptr;

    const auto color = resource_shape(color_resource);
    const auto depth = resource_shape(depth_resource);
    const auto velocity = resource_shape(velocity_resource);

    const auto color_invariant =
        color_resource != nullptr &&
        color_resource == post_main_resource &&
        color_resource == hdr_resource;
    const auto temporal_extents_aligned =
        same_extent(color, depth) &&
        same_extent(color, velocity);

    const auto view_ptr = reinterpret_cast<REManagedObject*>(
        m_latest_scene_view.load(std::memory_order_relaxed));
    const auto view_frame = m_latest_view_frame.load(std::memory_order_relaxed);
    const auto view_width = m_latest_view_width.load(std::memory_order_relaxed);
    const auto view_height = m_latest_view_height.load(std::memory_order_relaxed);
    const auto original_view_width = m_latest_original_view_width.load(std::memory_order_relaxed);
    const auto original_view_height = m_latest_original_view_height.load(std::memory_order_relaxed);
    const auto view_same_frame = frame.has_value() && view_frame != 0 && *frame == view_frame;

    D3D12Hook* d3d12{};
    IDXGISwapChain3* swapchain{};
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    HRESULT swap_desc_result = E_POINTER;
    UINT hook_display_width{};
    UINT hook_display_height{};
    UINT hook_render_width{};
    UINT hook_render_height{};
    uint32_t swapchain_source{};

    if (g_framework != nullptr) {
        const auto& hook = g_framework->get_d3d12_hook();
        d3d12 = hook.get();

        if (d3d12 != nullptr) {
            swapchain = d3d12->get_swap_chain();
            hook_display_width = d3d12->get_display_width();
            hook_display_height = d3d12->get_display_height();
            hook_render_width = d3d12->get_render_width();
            hook_render_height = d3d12->get_render_height();
            swapchain_source = static_cast<uint32_t>(d3d12->get_swapchain_source());

            if (swapchain != nullptr) {
                swap_desc_result = swapchain->GetDesc1(&swap_desc);
            }
        }
    }

    spdlog::info(
        "[RE4TemporalProbe] sizeSample={} scenario='{}' frame={} colorResource={:p} postMain={:p} hdr={:p} colorInvariant={} color={{width={},height={},format={},flags=0x{:x}}} depthResource={:p} depth={{width={},height={},format={},flags=0x{:x}}} velocityResource={:p} velocity={{width={},height={},format={},flags=0x{:x}}} temporalExtentsAligned={}",
        sample,
        scenario,
        frame.has_value() ? std::to_string(*frame) : "unknown",
        static_cast<void*>(color_resource),
        static_cast<void*>(post_main_resource),
        static_cast<void*>(hdr_resource),
        color_invariant,
        color.width,
        color.height,
        color.format,
        color.flags,
        static_cast<void*>(depth_resource),
        depth.width,
        depth.height,
        depth.format,
        depth.flags,
        static_cast<void*>(velocity_resource),
        velocity.width,
        velocity.height,
        velocity.format,
        velocity.flags,
        temporal_extents_aligned);

    spdlog::info(
        "[RE4TemporalProbe] sizeSample={} scenario='{}' engineView sceneView={:p} viewFrame={} sameFrame={} originalViewSize={}x{} overriddenViewSize={}x{}",
        sample,
        scenario,
        static_cast<void*>(view_ptr),
        view_frame,
        view_same_frame,
        original_view_width,
        original_view_height,
        view_width,
        view_height);

    spdlog::info(
        "[RE4TemporalProbe] sizeSample={} scenario='{}' dxgi swapchain={:p} source={} descResult=0x{:08x} swapSize={}x{} swapFormat={} bufferCount={} hookDisplay={}x{} hookRenderHint={}x{}",
        sample,
        scenario,
        static_cast<void*>(swapchain),
        swapchain_source,
        static_cast<uint32_t>(swap_desc_result),
        SUCCEEDED(swap_desc_result) ? swap_desc.Width : 0,
        SUCCEEDED(swap_desc_result) ? swap_desc.Height : 0,
        SUCCEEDED(swap_desc_result) ? static_cast<uint32_t>(swap_desc.Format) : 0,
        SUCCEEDED(swap_desc_result) ? swap_desc.BufferCount : 0,
        hook_display_width,
        hook_display_height,
        hook_render_width,
        hook_render_height);

    m_size_pair_sample.store(sample, std::memory_order_relaxed);
    m_size_pair_frame.store(frame.value_or(0), std::memory_order_relaxed);

    return true;
}

void RE4TemporalProbe::on_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) {
    if (!re4_temporal_probe::should_process_overlay(
            sdk::GameIdentity::get().is_re4(),
            m_enabled.load(std::memory_order_relaxed),
            layer,
            render_context)) {
        return;
    }

    const auto sample = m_size_pair_sample.load(std::memory_order_relaxed);
    const auto pre_frame = m_size_pair_frame.load(std::memory_order_relaxed);
    if (sample == 0 || pre_frame == 0) {
        return;
    }

    auto* renderer = sdk::renderer::get_renderer();
    const auto frame = renderer != nullptr ? renderer->get_render_frame() : std::nullopt;
    if (!frame.has_value() || *frame != pre_frame) {
        return;
    }

    const auto scenario = scenario_name(m_scenario.load(std::memory_order_relaxed));
    auto* context = static_cast<sdk::renderer::RenderContext*>(render_context);
    auto* current_target = context->get_render_target();
    auto* current_resource = current_target != nullptr ? current_target->get_native_resource_d3d12() : nullptr;
    auto* main_target = layer->get_main_target_state().get();
    auto* main_resource = main_target != nullptr ? main_target->get_native_resource_d3d12() : nullptr;

    const auto current = resource_shape(current_resource);
    const auto main = resource_shape(main_resource);

    spdlog::info(
        "[RE4TemporalProbe] sizeSamplePost={} scenario='{}' frame={} currentTarget={:p} currentResource={:p} current={{width={},height={},format={},flags=0x{:x}}} mainResource={:p} main={{width={},height={},format={},flags=0x{:x}}} currentMatchesMain={}",
        sample,
        scenario,
        *frame,
        static_cast<void*>(current_target),
        static_cast<void*>(current_resource),
        current.width,
        current.height,
        current.format,
        current.flags,
        static_cast<void*>(main_resource),
        main.width,
        main.height,
        main.format,
        main.flags,
        current_resource != nullptr && current_resource == main_resource);

    m_size_pair_sample.store(0, std::memory_order_relaxed);
    m_size_pair_frame.store(0, std::memory_order_relaxed);
}
