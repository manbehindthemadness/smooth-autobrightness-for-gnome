// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/frame_scheduler.h"

#include <assert.h>
#include <math.h>

unsigned int sabg_frame_scheduler_rate(
    double velocity,
    double remaining_distance,
    uint64_t remaining_usec,
    unsigned int minimum_hz,
    unsigned int maximum_hz,
    double maximum_step_per_frame
)
{
    double required_speed = fabs(velocity);
    double deadline_speed;
    double required_hz;

    assert(minimum_hz > 0);
    assert(minimum_hz <= maximum_hz);
    assert(maximum_step_per_frame > 0.0);

    remaining_distance = fabs(remaining_distance);
    if (remaining_distance > 0.0) {
        if (remaining_usec == 0)
            return maximum_hz;
        deadline_speed = remaining_distance * 1000000.0 / (double)remaining_usec;
        if (deadline_speed > required_speed)
            required_speed = deadline_speed;
    }

    required_hz = ceil(required_speed / maximum_step_per_frame);
    if (required_hz <= (double)minimum_hz)
        return minimum_hz;
    if (required_hz >= (double)maximum_hz)
        return maximum_hz;
    return (unsigned int)required_hz;
}
