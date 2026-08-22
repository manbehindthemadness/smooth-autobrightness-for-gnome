// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/ambient_model.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int round_percentage(double value)
{
    return (int)lround(value);
}

static void set_normalization(
    SabgAmbientModel *model,
    double lux,
    int percentage
)
{
    double safe_lux = lux > 0.01 ? lux : 0.01;
    int safe_percentage = percentage > 0 ? percentage : 1;

    model->normalization_lux = safe_lux * 100.0 / (double)safe_percentage;
}

void sabg_ambient_model_init(
    SabgAmbientModel *model,
    double initial_lux,
    int initial_percentage,
    double time_constant_seconds,
    int minimum_percentage,
    int maximum_percentage,
    uint64_t now_usec
)
{
    assert(model != NULL);
    assert(minimum_percentage >= 0);
    assert(maximum_percentage <= 100);
    assert(minimum_percentage <= maximum_percentage);

    model->minimum_percentage = minimum_percentage;
    model->maximum_percentage = maximum_percentage;
    model->time_constant_seconds = time_constant_seconds > 0.0
        ? time_constant_seconds
        : 0.001;
    model->filtered_percentage = clamp_double(
        (double)initial_percentage,
        (double)minimum_percentage,
        (double)maximum_percentage
    );
    model->last_lux = initial_lux;
    model->last_update_usec = now_usec;
    model->initialized = 1;
    set_normalization(model, initial_lux, initial_percentage);
}

int sabg_ambient_model_observe(
    SabgAmbientModel *model,
    double lux,
    uint64_t now_usec
)
{
    double desired;
    double elapsed_seconds;
    double alpha;

    assert(model != NULL);
    assert(model->initialized);

    model->last_lux = lux;
    desired = lux * 100.0 / model->normalization_lux;
    desired = clamp_double(
        desired,
        (double)model->minimum_percentage,
        (double)model->maximum_percentage
    );

    if (now_usec <= model->last_update_usec)
        return round_percentage(model->filtered_percentage);

    elapsed_seconds = (double)(now_usec - model->last_update_usec) / 1000000.0;
    alpha = 1.0 - exp(-elapsed_seconds / model->time_constant_seconds);
    model->filtered_percentage += alpha * (desired - model->filtered_percentage);
    model->last_update_usec = now_usec;

    return round_percentage(model->filtered_percentage);
}

void sabg_ambient_model_recalibrate(
    SabgAmbientModel *model,
    double lux,
    int manual_percentage,
    uint64_t now_usec
)
{
    assert(model != NULL);
    assert(model->initialized);

    manual_percentage = manual_percentage < model->minimum_percentage
        ? model->minimum_percentage
        : manual_percentage;
    manual_percentage = manual_percentage > model->maximum_percentage
        ? model->maximum_percentage
        : manual_percentage;

    set_normalization(model, lux, manual_percentage);
    model->filtered_percentage = (double)manual_percentage;
    model->last_lux = lux;
    model->last_update_usec = now_usec;
}
