// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/target_hysteresis.h"

#undef NDEBUG
#include <assert.h>

static void test_two_percent_band(void)
{
    SabgTargetHysteresis hysteresis;

    sabg_target_hysteresis_init(&hysteresis, 50, 0, 100, 2);
    assert(!sabg_target_hysteresis_accept(&hysteresis, 51));
    assert(hysteresis.accepted == 50);
    assert(sabg_target_hysteresis_accept(&hysteresis, 52));
    assert(!sabg_target_hysteresis_accept(&hysteresis, 51));
    assert(sabg_target_hysteresis_accept(&hysteresis, 49));
}

static void test_endpoints_remain_reachable_without_chatter(void)
{
    SabgTargetHysteresis hysteresis;

    sabg_target_hysteresis_init(&hysteresis, 1, 0, 100, 2);
    assert(sabg_target_hysteresis_accept(&hysteresis, 0));
    assert(!sabg_target_hysteresis_accept(&hysteresis, 1));
    assert(sabg_target_hysteresis_accept(&hysteresis, 2));

    sabg_target_hysteresis_reset(&hysteresis, 99);
    assert(sabg_target_hysteresis_accept(&hysteresis, 100));
    assert(!sabg_target_hysteresis_accept(&hysteresis, 99));
    assert(sabg_target_hysteresis_accept(&hysteresis, 98));
}

static void test_configuration_and_clamping(void)
{
    SabgTargetHysteresis hysteresis;

    sabg_target_hysteresis_init(&hysteresis, 50, 2, 90, 0);
    assert(sabg_target_hysteresis_accept(&hysteresis, 51));
    assert(sabg_target_hysteresis_accept(&hysteresis, -50));
    assert(hysteresis.accepted == 2);
    assert(sabg_target_hysteresis_accept(&hysteresis, 500));
    assert(hysteresis.accepted == 90);
}

int main(void)
{
    test_two_percent_band();
    test_endpoints_remain_reachable_without_chatter();
    test_configuration_and_clamping();
    return 0;
}

