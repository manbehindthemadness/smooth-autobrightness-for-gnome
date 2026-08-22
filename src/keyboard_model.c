// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/keyboard_model.h"

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

void sabg_keyboard_model_init(
    SabgKeyboardModel *model,
    double initial_lux,
    int initial_percentage,
    double time_constant_seconds,
    uint64_t now_usec
)
{
    int percentage;

    assert(model != NULL);
    percentage = clamp_percentage(initial_percentage);
    model->suspended_by_user = false;
    sabg_ambient_model_init(
        &model->inverse_ambient,
        initial_lux,
        100 - percentage,
        time_constant_seconds,
        0,
        100,
        now_usec
    );
}

bool sabg_keyboard_model_observe(
    SabgKeyboardModel *model,
    double lux,
    uint64_t now_usec,
    int *target_percentage
)
{
    assert(model != NULL);
    assert(target_percentage != NULL);

    if (model->suspended_by_user)
        return false;

    *target_percentage = 100 - sabg_ambient_model_observe(
        &model->inverse_ambient,
        lux,
        now_usec
    );
    return true;
}

void sabg_keyboard_model_manual_change(
    SabgKeyboardModel *model,
    double lux,
    int manual_percentage,
    uint64_t now_usec
)
{
    int percentage;

    assert(model != NULL);
    percentage = clamp_percentage(manual_percentage);
    if (percentage == 0) {
        model->suspended_by_user = true;
        return;
    }

    model->suspended_by_user = false;
    sabg_ambient_model_recalibrate(
        &model->inverse_ambient,
        lux,
        100 - percentage,
        now_usec
    );
}

