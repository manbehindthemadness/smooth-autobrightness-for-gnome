// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/smoother.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

static void test_short_transitions_keep_original_rate(void)
{
    SabgSmoother smoother;
    uint64_t now = UINT64_C(1000000);

    sabg_smoother_init(&smoother, 50, 40, 60, 250);
    assert(!sabg_smoother_active(&smoother));
    assert(!sabg_smoother_set_target(&smoother, 50, now));

    assert(sabg_smoother_set_target(&smoother, 53, now));
    assert(smoother.duration_usec == UINT64_C(120000));
    assert(smoother.frame_count == 3U);
    assert(sabg_smoother_next_wakeup_usec(&smoother) == now + UINT64_C(40000));
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(40000)) == 51);
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(80000)) == 52);
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(120000)) == 53);
    assert(!sabg_smoother_active(&smoother));

    now += UINT64_C(200000);
    assert(sabg_smoother_set_target(&smoother, 49, now));
    assert(smoother.duration_usec == UINT64_C(240000));
    assert(smoother.frame_count == 4U);
    assert(sabg_smoother_next_wakeup_usec(&smoother) == now + UINT64_C(60000));
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(60000)) == 52);
}

static void test_large_transition_is_capped_and_frame_limited(void)
{
    SabgSmoother smoother;
    uint64_t now = UINT64_C(2000000);
    int previous = 30;
    unsigned int frame;

    sabg_smoother_init(&smoother, 30, 40, 60, 250);
    assert(sabg_smoother_set_target(&smoother, 80, now));
    assert(smoother.duration_usec == UINT64_C(250000));
    assert(smoother.frame_count == 15U);

    for (frame = 1; frame <= smoother.frame_count; frame++) {
        uint64_t wakeup = sabg_smoother_next_wakeup_usec(&smoother);
        int current;

        assert(wakeup <= now + UINT64_C(250000));
        current = sabg_smoother_advance(&smoother, wakeup);
        assert(current > previous);
        previous = current;
    }
    assert(previous == 80);
    assert(!sabg_smoother_active(&smoother));
}

static void test_late_wakeup_and_retarget(void)
{
    SabgSmoother smoother;
    uint64_t now = UINT64_C(3000000);

    sabg_smoother_init(&smoother, 10, 40, 60, 250);
    assert(sabg_smoother_set_target(&smoother, 90, now));
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(125000)) == 47);

    now += UINT64_C(125000);
    assert(sabg_smoother_set_target(&smoother, 20, now));
    assert(smoother.start == 47);
    assert(smoother.duration_usec == UINT64_C(250000));
    assert(sabg_smoother_advance(&smoother, now + UINT64_C(250000)) == 20);
    assert(!sabg_smoother_active(&smoother));

    assert(sabg_smoother_set_target(&smoother, 1000, now));
    assert(smoother.target == 100);
    sabg_smoother_reset(&smoother, -20);
    assert(smoother.current == 0);
    assert(!sabg_smoother_active(&smoother));
}

static void test_zero_options_are_safely_clamped(void)
{
    SabgSmoother smoother;

    sabg_smoother_init(&smoother, 120, 0, 0, 0);
    assert(smoother.current == 100);
    assert(smoother.brighten_step_ms == 1U);
    assert(smoother.dim_step_ms == 1U);
    assert(smoother.maximum_transition_ms == 1U);
    assert(!sabg_smoother_set_target(&smoother, 101, 0));
}

int main(void)
{
    test_short_transitions_keep_original_rate();
    test_large_transition_is_capped_and_frame_limited();
    test_late_wakeup_and_retarget();
    test_zero_options_are_safely_clamped();
    return 0;
}
