// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

typedef struct {
    int output;
    int direction;
    int minimum;
    int maximum;
    unsigned int reversal_threshold;
} SabgOutputQuantizer;

void sabg_output_quantizer_init(
    SabgOutputQuantizer *quantizer,
    int current,
    int minimum,
    int maximum,
    unsigned int reversal_threshold
);

bool sabg_output_quantizer_update(
    SabgOutputQuantizer *quantizer,
    double position,
    int *output
);

void sabg_output_quantizer_reset(SabgOutputQuantizer *quantizer, int current);

