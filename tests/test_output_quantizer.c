// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/output_quantizer.h"

#undef NDEBUG
#include <assert.h>

int main(void)
{
    SabgOutputQuantizer quantizer;
    int output = -1;

    sabg_output_quantizer_init(&quantizer, 50, 0, 100, 2);
    assert(!sabg_output_quantizer_update(&quantizer, 50.6, &output));
    assert(sabg_output_quantizer_update(&quantizer, 51.6, &output));
    assert(output == 52);
    assert(sabg_output_quantizer_update(&quantizer, 52.6, &output));
    assert(output == 53);
    assert(!sabg_output_quantizer_update(&quantizer, 51.6, &output));
    assert(sabg_output_quantizer_update(&quantizer, 50.6, &output));
    assert(output == 51);

    sabg_output_quantizer_reset(&quantizer, 1);
    assert(sabg_output_quantizer_update(&quantizer, 0.4, &output));
    assert(output == 0);
    assert(!sabg_output_quantizer_update(&quantizer, 1.2, &output));
    assert(sabg_output_quantizer_update(&quantizer, 2.0, &output));
    assert(output == 2);
    return 0;
}

