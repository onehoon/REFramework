#pragma once

#include <cstdint>

#include <d3d12.h>

// Engine resources in this packet are borrowed only for the current
// RE4XeSS::on_pre_overlay_layer_draw callback.
struct RE4XeSSFrame {
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* velocity{};

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
    uint64_t frame_id{};
    uint64_t lifetime_trace_id{};
};
