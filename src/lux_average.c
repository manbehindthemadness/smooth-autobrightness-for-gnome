// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/lux_average.h"

#include <assert.h>
#include <string.h>

static void discard_first(SabgLuxAverage *average)
{
    assert(average->count > 0U);
    average->count--;
    memmove(
        average->timestamp_usec,
        average->timestamp_usec + 1,
        average->count * sizeof(average->timestamp_usec[0])
    );
    memmove(
        average->value,
        average->value + 1,
        average->count * sizeof(average->value[0])
    );
}

void sabg_lux_average_init(SabgLuxAverage *average, uint64_t window_usec)
{
    assert(average != NULL);
    assert(window_usec > 0U);
    average->window_usec = window_usec;
    average->current_average = 0.0;
    average->count = 0U;
}

void sabg_lux_average_clear(SabgLuxAverage *average)
{
    assert(average != NULL);
    average->current_average = 0.0;
    average->count = 0U;
}

void sabg_lux_average_reset(
    SabgLuxAverage *average,
    double lux,
    uint64_t now_usec
)
{
    assert(average != NULL);
    average->count = 1U;
    average->timestamp_usec[0] = now_usec;
    average->value[0] = lux;
    average->current_average = lux;
}

double sabg_lux_average_observe(
    SabgLuxAverage *average,
    double lux,
    uint64_t now_usec
)
{
    uint64_t cutoff_usec;
    uint64_t start_usec;
    uint64_t duration_usec;
    double integral = 0.0;
    size_t index;

    assert(average != NULL);
    if (average->count == 0U) {
        sabg_lux_average_reset(average, lux, now_usec);
        return lux;
    }
    if (now_usec <= average->timestamp_usec[average->count - 1U]) {
        average->value[average->count - 1U] = lux;
        return average->current_average;
    }
    if (average->count == SABG_LUX_AVERAGE_MAX_SAMPLES)
        discard_first(average);
    average->timestamp_usec[average->count] = now_usec;
    average->value[average->count] = lux;
    average->count++;

    cutoff_usec = now_usec > average->window_usec
        ? now_usec - average->window_usec
        : 0U;
    while (average->count > 1U && average->timestamp_usec[1] <= cutoff_usec)
        discard_first(average);

    start_usec = average->timestamp_usec[0] > cutoff_usec
        ? average->timestamp_usec[0]
        : cutoff_usec;
    duration_usec = now_usec - start_usec;
    if (duration_usec == 0U)
        return lux;

    for (index = 0U; index + 1U < average->count; index++) {
        uint64_t segment_start = average->timestamp_usec[index] > start_usec
            ? average->timestamp_usec[index]
            : start_usec;
        uint64_t segment_end = average->timestamp_usec[index + 1U];

        if (segment_end > segment_start) {
            integral += average->value[index]
                * (double)(segment_end - segment_start);
        }
    }
    average->current_average = integral / (double)duration_usec;
    return average->current_average;
}
