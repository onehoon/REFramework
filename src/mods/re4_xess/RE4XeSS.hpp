#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Mod.hpp"
#include "RE4XeSSFrame.hpp"
#include "RE4XeSSRuntime.hpp"

class RE4XeSS final : public Mod {
public:
    enum class UpscalingMode : int32_t {
        Off,
        NativeAA,
        UltraQualityPlus,
        UltraQuality,
        Quality,
        Balanced,
        Performance,
        UltraPerformance,
    };

    ~RE4XeSS() override;

    std::string_view get_name() const override {
        return "RE4XeSS";
    }

    std::optional<std::string> on_initialize() override;
    std::optional<std::string> on_initialize_d3d_thread() override;
    void on_frame() override;
    void on_draw_ui() override;
    void on_device_reset() override;
    void on_config_load(const utility::Config& cfg) override;
    void on_config_save(utility::Config& cfg) override;
    void on_view_get_size(REManagedObject* scene_view, float* result) override;
    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;

private:
    struct SceneInfoHistory {
        Matrix4x4f unjittered_projection{};
        Matrix4x4f view{};
        uint64_t frame_id{};
        bool valid{};
    };

    struct FrameSnapshot {
        uint64_t frame_id{};
        uintptr_t color_identity{};
        uintptr_t depth_identity{};
        uintptr_t velocity_identity{};
        uint32_t render_width{};
        uint32_t render_height{};
        uint32_t display_width{};
        uint32_t display_height{};
        float jitter_x_pixels{};
        float jitter_y_pixels{};
        float motion_scale_x{};
        float motion_scale_y{};
        float near_plane{};
        float far_plane{};
        float vertical_fov{};
        bool reset_history{};
    };

    void request_mode(UpscalingMode mode);
    void apply_pending_transition();
    void try_bootstrap();
    void update_temporal_configuration();
    void update_load_state();
    void invalidate_history(std::string_view reason, bool reset_jitter = true);
    void clear_frame_state();
    void clear_resource_identities();
    void reset_temporal_state(std::string_view reason, bool reset_load_state);
    bool get_display_resolution(xess_2d_t& resolution) const;
    bool is_temporal_active() const;

    UpscalingMode m_requested_mode{ UpscalingMode::Off };
    bool m_transition_pending{};
    bool m_bootstrap_pending{};
    ID3D12Device* m_device_identity{};
    std::string m_last_invalid_config_token{};
    RE4XeSSRuntime m_runtime{};

    bool m_temporal_ready{};
    bool m_temporal_signature_valid{};
    bool m_input_resolution_valid{};
    UpscalingMode m_temporal_mode{ UpscalingMode::Off };
    xess_quality_settings_t m_temporal_quality{ XESS_QUALITY_SETTING_AA };
    xess_2d_t m_display_resolution{};
    RE4XeSSRuntime::InputResolutionQuery m_input_resolution{};
    uint64_t m_temporal_generation{};
    uint32_t m_jitter_phase_count{ 8 };
    uint32_t m_next_jitter_sample{};
    std::string m_temporal_failure_reason{};

    bool m_history_invalid{ true };
    bool m_first_valid_frame_reset_pending{ true };
    std::string m_last_reset_reason{};
    std::array<SceneInfoHistory, 6> m_scene_history{};
    std::optional<uint64_t> m_last_processed_scene_frame{};
    std::optional<uint64_t> m_last_scene_callback_frame{};

    sdk::renderer::layer::Scene* m_cached_scene{};
    std::optional<uint64_t> m_cached_scene_frame{};
    float m_cached_jitter_x{};
    float m_cached_jitter_y{};
    float m_cached_vertical_fov{};
    float m_camera_near{};
    float m_camera_far{};
    std::optional<uint64_t> m_camera_frame{};
    bool m_camera_metadata_valid{};

    bool m_resource_identity_valid{};
    uintptr_t m_last_color_identity{};
    uintptr_t m_last_depth_identity{};
    uintptr_t m_last_velocity_identity{};
    std::optional<FrameSnapshot> m_latest_frame_snapshot{};

    bool m_pause_previous_valid{};
    bool m_pause_previous{};
    bool m_load_transition_active{};
    bool m_inhibit_departure_pending{};
    bool m_remembered_normal_inhibit_valid{};
    uint64_t m_remembered_normal_inhibit{};
    uint64_t m_departure_inhibit{};
    bool m_startup_mid_load{};
    bool m_post_pause_rebaseline_candidate_valid{};
    bool m_post_pause_rebaseline_transition_seen{};
    uint64_t m_post_pause_rebaseline_candidate{};
    uint32_t m_post_pause_rebaseline_stable_count{};
    bool m_load_observation_valid{};
};
