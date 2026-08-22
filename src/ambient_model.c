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

static double desired_percentage(const SabgAmbientModel *model, double lux)
{
    return clamp_double(
        lux * 100.0 / model->normalization_lux,
        (double)model->minimum_percentage,
        (double)model->maximum_percentage
    );
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
    model->dimming_time_constant_seconds = model->time_constant_seconds;
    model->dimming_finish_distance = 0.0;
    model->activity_threshold = 0.005;
    model->large_change_threshold = 0.0;
    model->large_change_time_constant_seconds = model->time_constant_seconds;
    model->large_change_finish_distance = 0.0;
    model->large_change_active = false;
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

void sabg_ambient_model_set_dimming_time_constant(
    SabgAmbientModel *model,
    double time_constant_seconds
)
{
    assert(model != NULL);
    model->dimming_time_constant_seconds = time_constant_seconds > 0.0
        ? time_constant_seconds
        : 0.001;
}

void sabg_ambient_model_set_dimming_finish_distance(
    SabgAmbientModel *model,
    double percentage
)
{
    assert(model != NULL);
    model->dimming_finish_distance = percentage > 0.0 ? percentage : 0.0;
}

void sabg_ambient_model_set_activity_threshold(
    SabgAmbientModel *model,
    double percentage
)
{
    assert(model != NULL);
    model->activity_threshold = percentage > 0.005 ? percentage : 0.005;
}

void sabg_ambient_model_set_large_change_response(
    SabgAmbientModel *model,
    double threshold_percentage,
    double time_constant_seconds,
    double finish_distance_percentage
)
{
    assert(model != NULL);
    model->large_change_threshold = threshold_percentage > 0.0
        ? threshold_percentage
        : 0.0;
    model->large_change_time_constant_seconds = time_constant_seconds > 0.0
        ? time_constant_seconds
        : 0.001;
    model->large_change_finish_distance = finish_distance_percentage > 0.0
        ? finish_distance_percentage
        : 0.0;
    model->large_change_active = false;
}

double sabg_ambient_model_advance(
    SabgAmbientModel *model,
    double lux,
    uint64_t now_usec
)
{
    double desired;
    double elapsed_seconds;
    double alpha;
    double time_constant;

    assert(model != NULL);
    assert(model->initialized);

    model->last_lux = lux;
    desired = desired_percentage(model, lux);

    if (now_usec <= model->last_update_usec)
        return model->filtered_percentage;

    elapsed_seconds = (double)(now_usec - model->last_update_usec) / 1000000.0;
    if (!model->large_change_active
        && model->large_change_threshold > 0.0
        && fabs(desired - model->filtered_percentage) >= model->large_change_threshold) {
        model->large_change_active = true;
    }
    if (model->large_change_active) {
        time_constant = model->large_change_time_constant_seconds;
    } else {
        time_constant = desired < model->filtered_percentage
            ? model->dimming_time_constant_seconds
            : model->time_constant_seconds;
    }
    alpha = 1.0 - exp(-elapsed_seconds / time_constant);
    model->filtered_percentage += alpha * (desired - model->filtered_percentage);
    if (model->large_change_active
        && fabs(model->filtered_percentage - desired)
            <= model->large_change_finish_distance) {
        model->filtered_percentage = desired;
        model->large_change_active = false;
    }
    if (desired < model->filtered_percentage
        && model->filtered_percentage - desired <= model->dimming_finish_distance) {
        model->filtered_percentage = desired;
    }
    model->last_update_usec = now_usec;

    return model->filtered_percentage;
}

int sabg_ambient_model_observe(
    SabgAmbientModel *model,
    double lux,
    uint64_t now_usec
)
{
    return round_percentage(sabg_ambient_model_advance(model, lux, now_usec));
}

bool sabg_ambient_model_active(const SabgAmbientModel *model)
{
    double desired;

    assert(model != NULL);
    assert(model->initialized);
    desired = desired_percentage(model, model->last_lux);
    return fabs(model->filtered_percentage - desired) >= model->activity_threshold;
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
    model->large_change_active = false;
}
