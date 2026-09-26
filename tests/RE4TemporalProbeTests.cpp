#include <cmath>
#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;

    int scene{};
    int camera{};
    float projection[16]{};

    CHECK(!should_process_scene(false, true, &scene));
    CHECK(!should_process_scene(true, false, &scene));
    CHECK(!should_process_scene(true, true, nullptr));
    CHECK(should_process_scene(true, true, &scene));

    CHECK(!should_process_camera_projection(false, true, &camera, projection));
    CHECK(!should_process_camera_projection(true, false, &camera, projection));
    CHECK(!should_process_camera_projection(true, true, nullptr, projection));
    CHECK(!should_process_camera_projection(true, true, &camera, nullptr));
    CHECK(should_process_camera_projection(true, true, &camera, projection));

    CHECK(MAX_TEMPORAL_SAMPLES == 32);
    CHECK(RESET_HISTORY_SCENARIO == 8);
    CHECK(LOAD_STATE_SCENARIO == 9);
    CHECK(EXECUTION_ORDER_SCENARIO == 10);
    CHECK(RESOURCE_STATE_SCENARIO == 11);
    CHECK(RESET_WATCH_MAX_SAMPLES == 4096);
    CHECK(LOAD_STATE_MAX_SAMPLES == 8192);
    CHECK(EXECUTION_ORDER_MAX_SAMPLES == 64);
    CHECK(RESOURCE_STATE_MAX_SAMPLES == 64);
    CHECK(JITTER_PHASE_COUNT == 4);
    CHECK(MV_READBACK_FIRST_SAMPLE == 5);
    CHECK(MV_READBACK_SAMPLE_COUNT == 16);
    CHECK(MV_READBACK_LAST_SAMPLE == 20);
    CHECK(!is_directional_mv_scenario(0));
    CHECK(is_directional_mv_scenario(1));
    CHECK(is_directional_mv_scenario(2));
    CHECK(is_directional_mv_scenario(3));
    CHECK(is_directional_mv_scenario(4));
    CHECK(!is_directional_mv_scenario(5));
    CHECK(!is_reset_history_scenario(7));
    CHECK(is_reset_history_scenario(8));
    CHECK(!is_reset_history_scenario(9));
    CHECK(!is_load_state_scenario(8));
    CHECK(is_load_state_scenario(9));
    CHECK(!is_load_state_scenario(10));
    CHECK(!is_execution_order_scenario(9));
    CHECK(is_execution_order_scenario(10));
    CHECK(!is_execution_order_scenario(11));
    CHECK(!is_resource_state_scenario(10));
    CHECK(is_resource_state_scenario(11));
    CHECK(!is_resource_state_scenario(12));
    CHECK(is_horizontal_mv_scenario(1));
    CHECK(is_horizontal_mv_scenario(2));
    CHECK(!is_horizontal_mv_scenario(3));
    CHECK(!is_horizontal_mv_scenario(4));
    CHECK(!is_vertical_mv_scenario(1));
    CHECK(!is_vertical_mv_scenario(2));
    CHECK(is_vertical_mv_scenario(3));
    CHECK(is_vertical_mv_scenario(4));
    CHECK(!should_readback_mv_sample(1, 4));
    CHECK(should_readback_mv_sample(1, 5));
    CHECK(should_readback_mv_sample(2, 20));
    CHECK(should_readback_mv_sample(3, 5));
    CHECK(should_readback_mv_sample(4, 20));
    CHECK(!should_readback_mv_sample(4, 21));
    CHECK(MV_SAMPLE_GRID_SIZE == 3);
    CHECK(MV_SAMPLE_POINT_COUNT == 9);
    CHECK(MV_READBACK_POINT_STRIDE == 512);
    CHECK(MV_READBACK_BUFFER_SIZE == 4608);

    const auto p0 = mv_sample_point(0, 2560, 1440);
    const auto p4 = mv_sample_point(4, 2560, 1440);
    const auto p8 = mv_sample_point(8, 2560, 1440);
    CHECK(p0.x == 640 && p0.y == 360);
    CHECK(p4.x == 1280 && p4.y == 720);
    CHECK(p8.x == 1920 && p8.y == 1080);

    CHECK(std::abs(decode_snorm16(0)) < 0.0000001f);
    CHECK(std::abs(decode_snorm16(32767) - 1.0f) < 0.0000001f);
    CHECK(std::abs(decode_snorm16(INT16_MIN) + 1.0f) < 0.0000001f);

    const auto candidate = historical_motion_pixel_candidate(0.01f, -0.02f, 2560, 1440);
    CHECK(std::abs(candidate.x - 12.8f) < 0.0001f);
    CHECK(std::abs(candidate.y - 14.4f) < 0.0001f);

    CHECK(!valid_clip_planes(0.0f, 1000.0f));
    CHECK(!valid_clip_planes(1.0f, 1.0f));
    CHECK(valid_clip_planes(0.01f, 10000.0f));

    const auto normal_depth = normal_depth_terms(0.01f, 10000.0f);
    const auto inverted_depth = inverted_depth_terms(0.01f, 10000.0f);
    CHECK(std::abs(normal_depth.p22 + 1.000001f) < 0.000001f);
    CHECK(std::abs(normal_depth.p32 + 0.01000001f) < 0.000001f);
    CHECK(std::abs(inverted_depth.p22 - 0.000001f) < 0.000001f);
    CHECK(std::abs(inverted_depth.p32 - 0.01000001f) < 0.000001f);
    CHECK(depth_terms_error(normal_depth, normal_depth) == 0.0f);
    CHECK(depth_terms_error(inverted_depth, normal_depth) > 1.0f);

    const ScreenPoint pixel{640.0f, 360.0f};
    const auto ndc = pixel_to_ndc(pixel, 2560, 1440);
    CHECK(std::abs(ndc.x + 0.5f) < 0.000001f);
    CHECK(std::abs(ndc.y - 0.5f) < 0.000001f);

    const auto roundtrip = ndc_to_pixel(ndc, 2560, 1440);
    CHECK(std::abs(roundtrip.x - pixel.x) < 0.0001f);
    CHECK(std::abs(roundtrip.y - pixel.y) < 0.0001f);

    const auto invalid_ndc = pixel_to_ndc(pixel, 0, 1440);
    const auto invalid_pixel = ndc_to_pixel(ndc, 2560, 0);
    CHECK(invalid_ndc.x == 0.0f && invalid_ndc.y == 0.0f);
    CHECK(invalid_pixel.x == 0.0f && invalid_pixel.y == 0.0f);

    const auto j1 = jitter_pixels_for_sample(1);
    const auto j2 = jitter_pixels_for_sample(2);
    const auto j3 = jitter_pixels_for_sample(3);
    const auto j4 = jitter_pixels_for_sample(4);
    const auto j5 = jitter_pixels_for_sample(5);

    CHECK(j1.x == 0.5f && j1.y == 0.5f);
    CHECK(j2.x == -0.5f && j2.y == 0.5f);
    CHECK(j3.x == -0.5f && j3.y == -0.5f);
    CHECK(j4.x == 0.5f && j4.y == -0.5f);
    CHECK(j5.x == j1.x && j5.y == j1.y);

    const auto matrix = projection_jitter_from_pixels(j1, 2560, 1440);
    CHECK(std::abs(matrix.x - 0.000390625f) < 0.000000001f);
    CHECK(std::abs(matrix.y + 0.0006944444f) < 0.00000001f);

    const auto zero = projection_jitter_from_pixels(j1, 0, 1440);
    CHECK(zero.x == 0.0f && zero.y == 0.0f);

    FrameBudget budget;
    CHECK(budget.sample_count() == 0);
    CHECK(budget.reserve_frame(0) == 0);

    CHECK(budget.reserve_frame(100) == 1);
    CHECK(budget.reserve_frame(100) == 0);
    CHECK(budget.reserve_frame(101) == 2);

    for (uint32_t i = 3; i <= MAX_TEMPORAL_SAMPLES; ++i) {
        CHECK(budget.reserve_frame(99 + i) == i);
    }

    CHECK(budget.reserve_frame(1000) == 0);
    CHECK(budget.sample_count() == MAX_TEMPORAL_SAMPLES);

    budget.reset();
    CHECK(budget.sample_count() == 0);
    CHECK(budget.reserve_frame(2000) == 1);

    FrameBudget long_budget;
    CHECK(long_budget.reserve_frame(3000, 2) == 1);
    CHECK(long_budget.reserve_frame(3001, 2) == 2);
    CHECK(long_budget.reserve_frame(3002, 2) == 0);
    CHECK(long_budget.sample_count() == 2);

    return 0;
}
