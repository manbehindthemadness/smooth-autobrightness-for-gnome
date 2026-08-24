// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SABG_LUX_AVERAGE_MAX_SAMPLES 256U

typedef struct {
    uint64_t timestamp_usec[SABG_LUX_AVERAGE_MAX_SAMPLES];
    double value[SABG_LUX_AVERAGE_MAX_SAMPLES];
    double current_average;
    uint64_t window_usec;
    size_t count;
} SabgLuxAverage;

void sabg_lux_average_init(SabgLuxAverage *average, uint64_t window_usec);
void sabg_lux_average_clear(SabgLuxAverage *average);
void sabg_lux_average_reset(
    SabgLuxAverage *average,
    double lux,
    uint64_t now_usec
);
double sabg_lux_average_observe(
    SabgLuxAverage *average,
    double lux,
    uint64_t now_usec
);
