// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double normalization_lux;
    double filtered_percentage;
    double filtered_velocity;
    double last_lux;
    double time_constant_seconds;
    double dimming_time_constant_seconds;
    double dimming_finish_distance;
    double activity_threshold;
    double large_change_threshold;
    double large_change_time_constant_seconds;
    double large_change_finish_distance;
    uint64_t last_update_usec;
    int minimum_percentage;
    int maximum_percentage;
    int initialized;
    bool large_change_active;
} SabgAmbientModel;

void sabg_ambient_model_set_dimming_time_constant(
    SabgAmbientModel *model,
    double time_constant_seconds
);

void sabg_ambient_model_set_dimming_finish_distance(
    SabgAmbientModel *model,
    double percentage
);

void sabg_ambient_model_set_activity_threshold(
    SabgAmbientModel *model,
    double percentage
);

void sabg_ambient_model_set_large_change_response(
    SabgAmbientModel *model,
    double threshold_percentage,
    double time_constant_seconds,
    double finish_distance_percentage
);

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

double sabg_ambient_model_advance(
    SabgAmbientModel *model,
    double lux,
    uint64_t now_usec
);

bool sabg_ambient_model_active(const SabgAmbientModel *model);
double sabg_ambient_model_velocity(const SabgAmbientModel *model);

void sabg_ambient_model_recalibrate(
    SabgAmbientModel *model,
    double lux,
    int manual_percentage,
    uint64_t now_usec
);

void sabg_ambient_model_resume(
    SabgAmbientModel *model,
    double lux,
    int current_percentage,
    uint64_t now_usec
);
