// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sabg/ambient_model.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    SabgAmbientModel inverse_ambient;
    bool suspended_by_user;
} SabgKeyboardModel;

void sabg_keyboard_model_init(
    SabgKeyboardModel *model,
    double initial_lux,
    int initial_percentage,
    double time_constant_seconds,
    uint64_t now_usec
);

bool sabg_keyboard_model_observe(
    SabgKeyboardModel *model,
    double lux,
    uint64_t now_usec,
    int *target_percentage
);

bool sabg_keyboard_model_advance(
    SabgKeyboardModel *model,
    double lux,
    uint64_t now_usec,
    double *target_percentage
);

bool sabg_keyboard_model_active(const SabgKeyboardModel *model);
double sabg_keyboard_model_velocity(const SabgKeyboardModel *model);

void sabg_keyboard_model_set_activity_threshold(
    SabgKeyboardModel *model,
    double percentage
);

void sabg_keyboard_model_set_large_change_response(
    SabgKeyboardModel *model,
    double threshold_percentage,
    double time_constant_seconds,
    double finish_distance_percentage
);

void sabg_keyboard_model_manual_change(
    SabgKeyboardModel *model,
    double lux,
    int manual_percentage,
    uint64_t now_usec
);
