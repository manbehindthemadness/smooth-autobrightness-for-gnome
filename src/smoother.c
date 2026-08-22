// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/smoother.h"

#include <assert.h>
#include <stddef.h>

#define MAXIMUM_UPDATE_HZ UINT64_C(60)

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
    unsigned int dim_step_ms,
    unsigned int maximum_transition_ms
)
{
    assert(smoother != NULL);

    smoother->current = clamp_percentage(current);
    smoother->target = smoother->current;
    smoother->start = smoother->current;
    smoother->brighten_step_ms = brighten_step_ms > 0 ? brighten_step_ms : 1U;
    smoother->dim_step_ms = dim_step_ms > 0 ? dim_step_ms : 1U;
    smoother->maximum_transition_ms = maximum_transition_ms > 0
        ? maximum_transition_ms
        : 1U;
    smoother->frame_count = 0;
    smoother->frame_index = 0;
    smoother->start_usec = 0;
    smoother->duration_usec = 0;
}

bool sabg_smoother_set_target(SabgSmoother *smoother, int target, uint64_t now_usec)
{
    int clamped;
    unsigned int difference;
    unsigned int step_ms;
    uint64_t natural_duration_usec;
    uint64_t maximum_duration_usec;
    uint64_t maximum_frames;

    assert(smoother != NULL);
    clamped = clamp_percentage(target);
    if (clamped == smoother->target)
        return false;

    smoother->start = smoother->current;
    smoother->target = clamped;
    smoother->start_usec = now_usec;
    smoother->frame_index = 0;

    difference = (unsigned int)(clamped > smoother->current
        ? clamped - smoother->current
        : smoother->current - clamped);
    if (difference == 0U) {
        smoother->frame_count = 0;
        smoother->duration_usec = 0;
        return true;
    }

    step_ms = clamped > smoother->current
        ? smoother->brighten_step_ms
        : smoother->dim_step_ms;
    natural_duration_usec = (uint64_t)difference * (uint64_t)step_ms * UINT64_C(1000);
    maximum_duration_usec = (uint64_t)smoother->maximum_transition_ms * UINT64_C(1000);
    smoother->duration_usec = natural_duration_usec < maximum_duration_usec
        ? natural_duration_usec
        : maximum_duration_usec;

    maximum_frames = (smoother->duration_usec * MAXIMUM_UPDATE_HZ
        + UINT64_C(999999)) / UINT64_C(1000000);
    if (maximum_frames == 0)
        maximum_frames = 1;
    smoother->frame_count = difference < maximum_frames
        ? difference
        : (unsigned int)maximum_frames;
    return true;
}

bool sabg_smoother_active(const SabgSmoother *smoother)
{
    assert(smoother != NULL);
    return smoother->current != smoother->target;
}

int sabg_smoother_advance(SabgSmoother *smoother, uint64_t now_usec)
{
    uint64_t elapsed_usec;
    uint64_t calculated_index;
    unsigned int difference;
    unsigned int moved;

    assert(smoother != NULL);

    if (!sabg_smoother_active(smoother))
        return smoother->current;

    elapsed_usec = now_usec > smoother->start_usec
        ? now_usec - smoother->start_usec
        : 0;
    calculated_index = elapsed_usec >= smoother->duration_usec
        ? smoother->frame_count
        : elapsed_usec * smoother->frame_count / smoother->duration_usec;
    if (calculated_index <= smoother->frame_index)
        calculated_index = (uint64_t)smoother->frame_index + 1U;
    if (calculated_index > smoother->frame_count)
        calculated_index = smoother->frame_count;
    smoother->frame_index = (unsigned int)calculated_index;

    difference = (unsigned int)(smoother->target > smoother->start
        ? smoother->target - smoother->start
        : smoother->start - smoother->target);
    moved = (difference * smoother->frame_index + smoother->frame_count / 2U)
        / smoother->frame_count;
    smoother->current = smoother->target > smoother->start
        ? smoother->start + (int)moved
        : smoother->start - (int)moved;
    if (smoother->frame_index == smoother->frame_count)
        smoother->current = smoother->target;

    return smoother->current;
}

uint64_t sabg_smoother_next_wakeup_usec(const SabgSmoother *smoother)
{
    assert(smoother != NULL);
    assert(sabg_smoother_active(smoother));
    assert(smoother->frame_count > 0U);

    return smoother->start_usec
        + smoother->duration_usec * ((uint64_t)smoother->frame_index + 1U)
            / smoother->frame_count;
}

void sabg_smoother_reset(SabgSmoother *smoother, int current)
{
    assert(smoother != NULL);

    smoother->current = clamp_percentage(current);
    smoother->target = smoother->current;
    smoother->start = smoother->current;
    smoother->frame_count = 0;
    smoother->frame_index = 0;
    smoother->start_usec = 0;
    smoother->duration_usec = 0;
}
