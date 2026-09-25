#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;
    int scene{};

    CHECK(!should_process_scene(false, &scene));
    CHECK(!should_process_scene(true, nullptr));
    CHECK(should_process_scene(true, &scene));

    CHECK(!is_primary_scene(false, true, true));
    CHECK(!is_primary_scene(true, false, true));
    CHECK(!is_primary_scene(true, true, false));
    CHECK(is_primary_scene(true, true, true));
    CHECK(!has_scene_local_color_candidate(true, nullptr));
    CHECK(!has_scene_local_color_candidate(false, &scene));
    CHECK(has_scene_local_color_candidate(true, &scene));

    SampleBudget budget;
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);
    CHECK(budget.should_sample_callback());
    for (uint32_t callback = 1; callback < SAMPLE_INTERVAL_CALLBACKS; ++callback) {
        CHECK(!budget.should_sample_callback());
    }
    CHECK(budget.callback_count() == SAMPLE_INTERVAL_CALLBACKS);

    for (uint32_t sample = 1; sample <= MAX_SAMPLES; ++sample) {
        CHECK(budget.reserve_sample() == sample);
    }
    CHECK(budget.reserve_sample() == 0);
    CHECK(budget.sample_count() == MAX_SAMPLES);

    budget.reset();
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);
    CHECK(budget.should_sample_callback());
    CHECK(budget.reserve_sample() == 1);

    return 0;
}
