// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

unsigned int sabg_frame_scheduler_rate(
    double velocity,
    double remaining_distance,
    uint64_t remaining_usec,
    unsigned int minimum_hz,
    unsigned int maximum_hz,
    double maximum_step_per_frame
);
