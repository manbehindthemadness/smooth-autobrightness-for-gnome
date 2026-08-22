// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

typedef struct {
    int accepted;
    int minimum;
    int maximum;
    unsigned int threshold;
} SabgTargetHysteresis;

void sabg_target_hysteresis_init(
    SabgTargetHysteresis *hysteresis,
    int current,
    int minimum,
    int maximum,
    unsigned int threshold
);

bool sabg_target_hysteresis_accept(
    SabgTargetHysteresis *hysteresis,
    int candidate
);

void sabg_target_hysteresis_reset(
    SabgTargetHysteresis *hysteresis,
    int current
);

