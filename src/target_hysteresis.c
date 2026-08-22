// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/target_hysteresis.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>

static int clamp_target(const SabgTargetHysteresis *hysteresis, int target)
{
    if (target < hysteresis->minimum)
        return hysteresis->minimum;
    if (target > hysteresis->maximum)
        return hysteresis->maximum;
    return target;
}

void sabg_target_hysteresis_init(
    SabgTargetHysteresis *hysteresis,
    int current,
    int minimum,
    int maximum,
    unsigned int threshold
)
{
    assert(hysteresis != NULL);
    assert(minimum <= maximum);

    hysteresis->minimum = minimum;
    hysteresis->maximum = maximum;
    hysteresis->threshold = threshold;
    hysteresis->accepted = clamp_target(hysteresis, current);
}

bool sabg_target_hysteresis_accept(
    SabgTargetHysteresis *hysteresis,
    int candidate
)
{
    int difference;

    assert(hysteresis != NULL);
    candidate = clamp_target(hysteresis, candidate);
    if (candidate == hysteresis->accepted)
        return false;

    difference = abs(candidate - hysteresis->accepted);
    if (candidate != hysteresis->minimum
        && candidate != hysteresis->maximum
        && (unsigned int)difference < hysteresis->threshold) {
        return false;
    }

    hysteresis->accepted = candidate;
    return true;
}

void sabg_target_hysteresis_reset(
    SabgTargetHysteresis *hysteresis,
    int current
)
{
    assert(hysteresis != NULL);
    hysteresis->accepted = clamp_target(hysteresis, current);
}

