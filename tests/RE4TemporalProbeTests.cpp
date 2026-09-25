#include <cstdint>

#include "RE4TemporalProbeSupport.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    using namespace re4_temporal_probe;
    int scene{};
    int post_effect{};
    int overlay{};
    int render_context{};

    CHECK(!should_process_scene(false, true, &scene));
    CHECK(!should_process_scene(true, false, &scene));
    CHECK(!should_process_scene(true, true, nullptr));
    CHECK(should_process_scene(true, true, &scene));

    CHECK(!should_process_post_effect(false, true, &post_effect, &render_context));
    CHECK(!should_process_post_effect(true, false, &post_effect, &render_context));
    CHECK(!should_process_post_effect(true, true, nullptr, &render_context));
    CHECK(!should_process_post_effect(true, true, &post_effect, nullptr));
    CHECK(should_process_post_effect(true, true, &post_effect, &render_context));

    CHECK(!should_process_overlay(false, true, &overlay, &render_context));
    CHECK(!should_process_overlay(true, false, &overlay, &render_context));
    CHECK(!should_process_overlay(true, true, nullptr, &render_context));
    CHECK(!should_process_overlay(true, true, &overlay, nullptr));
    CHECK(should_process_overlay(true, true, &overlay, &render_context));

    CHECK(!is_primary_scene(false, true, true));
    CHECK(!is_primary_scene(true, false, true));
    CHECK(!is_primary_scene(true, true, false));
    CHECK(is_primary_scene(true, true, true));

    CHECK(MAX_SAMPLES == 10);
    CHECK(MAX_SCENE_RTVS == 8);
    CHECK(MAX_POST_EFFECT_RTVS == 8);
    CHECK(MAX_OVERLAY_RTVS == 8);

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
