// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/smoother.h"

#include <assert.h>
#include <stddef.h>

static int clamp_percentage(int value)
{
    if (value < 0)
        return 0;
    if (value > 100)
        return 100;
    return value;
}

void sabg_smoother_init(
    SabgSmoother *smoother,
    int current,
    unsigned int brighten_step_ms,
    unsigned int dim_step_ms
)
{
    assert(smoother != NULL);

    smoother->current = clamp_percentage(current);
    smoother->target = smoother->current;
    smoother->brighten_step_ms = brighten_step_ms > 0 ? brighten_step_ms : 1U;
    smoother->dim_step_ms = dim_step_ms > 0 ? dim_step_ms : 1U;
}

bool sabg_smoother_set_target(SabgSmoother *smoother, int target)
{
    int clamped;

    assert(smoother != NULL);
    clamped = clamp_percentage(target);
    if (clamped == smoother->target)
        return false;

    smoother->target = clamped;
    return true;
}

bool sabg_smoother_active(const SabgSmoother *smoother)
{
    assert(smoother != NULL);
    return smoother->current != smoother->target;
}

int sabg_smoother_advance(SabgSmoother *smoother)
{
    assert(smoother != NULL);

    if (smoother->current < smoother->target)
        smoother->current++;
    else if (smoother->current > smoother->target)
        smoother->current--;

    return smoother->current;
}

uint64_t sabg_smoother_next_delay_usec(const SabgSmoother *smoother)
{
    unsigned int milliseconds;

    assert(smoother != NULL);
    milliseconds = smoother->target > smoother->current
        ? smoother->brighten_step_ms
        : smoother->dim_step_ms;
    return (uint64_t)milliseconds * UINT64_C(1000);
}
