// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

typedef struct {
    double normalization_lux;
    double filtered_percentage;
    double last_lux;
    double time_constant_seconds;
    uint64_t last_update_usec;
    int minimum_percentage;
    int maximum_percentage;
    int initialized;
} SabgAmbientModel;

void sabg_ambient_model_init(
    SabgAmbientModel *model,
    double initial_lux,
    int initial_percentage,
    double time_constant_seconds,
    int minimum_percentage,
    int maximum_percentage,
    uint64_t now_usec
);
int sabg_ambient_model_observe(
    SabgAmbientModel *model,
    double lux,
    uint64_t now_usec
);

void sabg_ambient_model_recalibrate(
    SabgAmbientModel *model,
    double lux,
    int manual_percentage,
    uint64_t now_usec
);
