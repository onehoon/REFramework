#pragma once

#include <string>
#include <string_view>

#include "Mod.hpp"
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

private:
    void request_mode(UpscalingMode mode);
    void apply_pending_transition();
    void try_bootstrap();

    UpscalingMode m_requested_mode{ UpscalingMode::Off };
    bool m_transition_pending{};
    bool m_bootstrap_pending{};
    ID3D12Device* m_device_identity{};
    std::string m_last_invalid_config_token{};
    RE4XeSSRuntime m_runtime{};
};
