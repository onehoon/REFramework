#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;

    int scene{};
    int camera{};
    float projection[16]{};

    CHECK(!should_process_scene_update(false, true, &scene));
    CHECK(!should_process_scene_update(true, false, &scene));
    CHECK(!should_process_scene_update(true, true, nullptr));
    CHECK(should_process_scene_update(true, true, &scene));

    CHECK(!should_process_camera_projection(false, true, &camera, projection));
    CHECK(!should_process_camera_projection(true, false, &camera, projection));
    CHECK(!should_process_camera_projection(true, true, nullptr, projection));
    CHECK(!should_process_camera_projection(true, true, &camera, nullptr));
    CHECK(should_process_camera_projection(true, true, &camera, projection));

    CHECK(MAX_TEMPORAL_SAMPLES == 32);

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
