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
    CHECK(JITTER_PHASE_COUNT == 4);

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

    return 0;
}
