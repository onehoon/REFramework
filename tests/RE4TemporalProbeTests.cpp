#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;
    int scene{};

    CHECK(!should_process_scene(false, true, &scene));
    CHECK(!should_process_scene(true, false, &scene));
    CHECK(!should_process_scene(true, true, nullptr));
    CHECK(should_process_scene(true, true, &scene));

    CHECK(!should_process_end_rendering(false, true, "EndRendering"));
    CHECK(!should_process_end_rendering(true, false, "EndRendering"));
    CHECK(!should_process_end_rendering(true, true, "BeginRendering"));
    CHECK(should_process_end_rendering(true, true, "EndRendering"));

    CHECK(!is_primary_scene(false, true, true));
    CHECK(!is_primary_scene(true, false, true));
    CHECK(!is_primary_scene(true, true, false));
    CHECK(is_primary_scene(true, true, true));
    CHECK(!has_scene_local_color_candidate(true, nullptr));
    CHECK(!has_scene_local_color_candidate(false, &scene));
    CHECK(has_scene_local_color_candidate(true, &scene));

    SampleBudget interleaved_budget;
    uint32_t interleaved_captures{};
    for (uint32_t frame = 0; frame < SAMPLE_INTERVAL_CALLBACKS * 2; ++frame) {
        CHECK(!should_sample_primary_scene(false, interleaved_budget));
        const auto capture = should_sample_primary_scene(true, interleaved_budget);
        if (capture) {
            CHECK(interleaved_budget.reserve_sample() != 0);
            ++interleaved_captures;
        }
    }
    CHECK(interleaved_budget.callback_count() == SAMPLE_INTERVAL_CALLBACKS * 2);
    CHECK(interleaved_captures == 2);

    SampleBudget budget;
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);

    for (uint32_t sample = 1; sample <= MAX_SAMPLES; ++sample) {
        CHECK(budget.reserve_sample() == sample);
    }
    CHECK(budget.reserve_sample() == 0);
    CHECK(budget.sample_count() == MAX_SAMPLES);

    budget.reset();
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);
    CHECK(should_sample_primary_scene(true, budget));
    CHECK(budget.reserve_sample() == 1);

    return 0;
}
