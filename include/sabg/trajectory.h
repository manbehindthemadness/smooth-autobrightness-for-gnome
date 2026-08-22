// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double position;
    double velocity;
    double target;
    double omega;
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
    unsigned int maximum_transition_ms;
    uint64_t last_update_usec;
    uint64_t deadline_usec;
    int minimum;
    int maximum;
} SabgTrajectory;

void sabg_trajectory_init(
    SabgTrajectory *trajectory,
    double current,
    unsigned int brighten_step_ms,
    unsigned int dim_step_ms,
    unsigned int maximum_transition_ms,
    int minimum,
    int maximum,
    uint64_t now_usec
);

void sabg_trajectory_set_target(
    SabgTrajectory *trajectory,
    double target,
    uint64_t now_usec
);

double sabg_trajectory_advance(SabgTrajectory *trajectory, uint64_t now_usec);
bool sabg_trajectory_active(const SabgTrajectory *trajectory);
void sabg_trajectory_reset(SabgTrajectory *trajectory, double current, uint64_t now_usec);

