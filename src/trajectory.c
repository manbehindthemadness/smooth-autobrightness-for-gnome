// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/trajectory.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>

#define MINIMUM_RESPONSE_USEC UINT64_C(100000)
#define SETTLING_CONSTANT 8.0

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

void sabg_trajectory_init(
    SabgTrajectory *trajectory,
    double current,
    unsigned int brighten_step_ms,
    unsigned int dim_step_ms,
    unsigned int maximum_transition_ms,
    double minimum,
    double maximum,
    uint64_t now_usec
)
{
    assert(trajectory != NULL);
    assert(minimum <= maximum);

    trajectory->minimum = minimum;
    trajectory->maximum = maximum;
    trajectory->position = clamp_double(current, minimum, maximum);
    trajectory->velocity = 0.0;
    trajectory->target = trajectory->position;
    trajectory->omega = 0.0;
    trajectory->brighten_step_ms = brighten_step_ms > 0 ? brighten_step_ms : 1U;
    trajectory->dim_step_ms = dim_step_ms > 0 ? dim_step_ms : 1U;
    trajectory->maximum_transition_ms = maximum_transition_ms > 0
        ? maximum_transition_ms
        : 1U;
    trajectory->last_update_usec = now_usec;
    trajectory->deadline_usec = now_usec;
}

double sabg_trajectory_advance(SabgTrajectory *trajectory, uint64_t now_usec)
{
    double old_offset;
    double coefficient;
    double elapsed_seconds;
    double exponential;
    double new_offset;

    assert(trajectory != NULL);
    if (now_usec <= trajectory->last_update_usec)
        return trajectory->position;
    if (!sabg_trajectory_active(trajectory)) {
        trajectory->last_update_usec = now_usec;
        return trajectory->position;
    }

    if (now_usec >= trajectory->deadline_usec) {
        trajectory->position = trajectory->target;
        trajectory->velocity = 0.0;
        trajectory->last_update_usec = now_usec;
        return trajectory->position;
    }

    elapsed_seconds = (double)(now_usec - trajectory->last_update_usec) / 1000000.0;
    old_offset = trajectory->position - trajectory->target;
    coefficient = trajectory->velocity + trajectory->omega * old_offset;
    exponential = exp(-trajectory->omega * elapsed_seconds);
    new_offset = (old_offset + coefficient * elapsed_seconds) * exponential;
    trajectory->velocity = (
        trajectory->velocity - trajectory->omega * coefficient * elapsed_seconds
    ) * exponential;
    trajectory->position = clamp_double(
        trajectory->target + new_offset,
        trajectory->minimum,
        trajectory->maximum
    );
    if ((old_offset < 0.0 && new_offset >= 0.0)
        || (old_offset > 0.0 && new_offset <= 0.0)) {
        trajectory->position = trajectory->target;
        trajectory->velocity = 0.0;
    }
    trajectory->last_update_usec = now_usec;
    return trajectory->position;
}

void sabg_trajectory_set_target(
    SabgTrajectory *trajectory,
    double target,
    uint64_t now_usec
)
{
    double distance;
    unsigned int step_ms;
    uint64_t duration_usec;
    uint64_t maximum_usec;

    assert(trajectory != NULL);
    (void)sabg_trajectory_advance(trajectory, now_usec);
    target = clamp_double(target, trajectory->minimum, trajectory->maximum);
    if (fabs(target - trajectory->target) < 0.0001)
        return;

    trajectory->target = target;
    distance = fabs(target - trajectory->position);
    step_ms = target > trajectory->position
        ? trajectory->brighten_step_ms
        : trajectory->dim_step_ms;
    duration_usec = (uint64_t)(distance * (double)step_ms * 1000.0);
    if (duration_usec < MINIMUM_RESPONSE_USEC)
        duration_usec = MINIMUM_RESPONSE_USEC;
    maximum_usec = (uint64_t)trajectory->maximum_transition_ms * UINT64_C(1000);
    if (duration_usec > maximum_usec)
        duration_usec = maximum_usec;
    if (duration_usec == 0)
        duration_usec = 1;
    trajectory->omega = SETTLING_CONSTANT * 1000000.0 / (double)duration_usec;
    trajectory->deadline_usec = now_usec + duration_usec;
}

bool sabg_trajectory_active(const SabgTrajectory *trajectory)
{
    assert(trajectory != NULL);
    return fabs(trajectory->position - trajectory->target) >= 0.005
        || fabs(trajectory->velocity) >= 0.05;
}

void sabg_trajectory_reset(SabgTrajectory *trajectory, double current, uint64_t now_usec)
{
    assert(trajectory != NULL);
    trajectory->position = clamp_double(current, trajectory->minimum, trajectory->maximum);
    trajectory->velocity = 0.0;
    trajectory->target = trajectory->position;
    trajectory->omega = 0.0;
    trajectory->last_update_usec = now_usec;
    trajectory->deadline_usec = now_usec;
}
