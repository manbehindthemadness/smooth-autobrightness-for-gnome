// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/output_quantizer.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static int clamp_output(const SabgOutputQuantizer *quantizer, int value)
{
    if (value < quantizer->minimum)
        return quantizer->minimum;
    if (value > quantizer->maximum)
        return quantizer->maximum;
    return value;
}

void sabg_output_quantizer_init(
    SabgOutputQuantizer *quantizer,
    int current,
    int minimum,
    int maximum,
    unsigned int reversal_threshold
)
{
    assert(quantizer != NULL);
    assert(minimum <= maximum);
    quantizer->minimum = minimum;
    quantizer->maximum = maximum;
    quantizer->reversal_threshold = reversal_threshold;
    quantizer->output = clamp_output(quantizer, current);
    quantizer->direction = 0;
}

bool sabg_output_quantizer_update(
    SabgOutputQuantizer *quantizer,
    double position,
    int *output
)
{
    int candidate;
    int difference;
    int direction;

    assert(quantizer != NULL);
    assert(output != NULL);
    candidate = clamp_output(quantizer, (int)lround(position));
    if (candidate == quantizer->output)
        return false;

    difference = candidate - quantizer->output;
    direction = difference > 0 ? 1 : -1;
    if (candidate != quantizer->minimum
        && candidate != quantizer->maximum
        && direction != quantizer->direction
        && (unsigned int)abs(difference) < quantizer->reversal_threshold) {
        return false;
    }

    quantizer->output = candidate;
    quantizer->direction = direction;
    *output = candidate;
    return true;
}

void sabg_output_quantizer_reset(SabgOutputQuantizer *quantizer, int current)
{
    assert(quantizer != NULL);
    quantizer->output = clamp_output(quantizer, current);
    quantizer->direction = 0;
}

