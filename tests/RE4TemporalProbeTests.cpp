#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;

    int overlay{};
    int render_context{};
    int scene_view{};
    float view_size[2]{1920.0f, 1080.0f};

    CHECK(!should_process_overlay(false, true, &overlay, &render_context));
    CHECK(!should_process_overlay(true, false, &overlay, &render_context));
    CHECK(!should_process_overlay(true, true, nullptr, &render_context));
    CHECK(!should_process_overlay(true, true, &overlay, nullptr));
    CHECK(should_process_overlay(true, true, &overlay, &render_context));

    CHECK(!should_process_view_size(false, true, &scene_view, view_size));
    CHECK(!should_process_view_size(true, false, &scene_view, view_size));
    CHECK(!should_process_view_size(true, true, nullptr, view_size));
    CHECK(!should_process_view_size(true, true, &scene_view, nullptr));
    CHECK(should_process_view_size(true, true, &scene_view, view_size));

    CHECK(MAX_SAMPLES == 10);

    SampleBudget budget;
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);

    uint32_t captures{};
    for (uint32_t callback = 0; callback < SAMPLE_INTERVAL_CALLBACKS * 2; ++callback) {
        if (budget.should_sample_callback()) {
            CHECK(budget.reserve_sample() != 0);
            ++captures;
        }
    }

    CHECK(budget.callback_count() == SAMPLE_INTERVAL_CALLBACKS * 2);
    CHECK(captures == 2);
    CHECK(budget.sample_count() == 2);

    budget.reset();
    CHECK(budget.callback_count() == 0);
    CHECK(budget.sample_count() == 0);

    for (uint32_t sample = 1; sample <= MAX_SAMPLES; ++sample) {
        CHECK(budget.reserve_sample() == sample);
    }

    CHECK(budget.reserve_sample() == 0);
    CHECK(budget.sample_count() == MAX_SAMPLES);

    return 0;
}
