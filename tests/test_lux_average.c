// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/lux_average.h"

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>

int main(void)
{
    SabgLuxAverage average;
    double lux;

    sabg_lux_average_init(&average, UINT64_C(5000000));
    lux = sabg_lux_average_observe(&average, 0.0, UINT64_C(1000000));
    assert(lux == 0.0);
    lux = sabg_lux_average_observe(&average, 10.0, UINT64_C(2000000));
    assert(lux == 0.0);
    lux = sabg_lux_average_observe(&average, 10.0, UINT64_C(3000000));
    assert(fabs(lux - 5.0) < 0.000001);
    lux = sabg_lux_average_observe(&average, 10.0, UINT64_C(6000000));
    assert(fabs(lux - 8.0) < 0.000001);
    lux = sabg_lux_average_observe(&average, 10.0, UINT64_C(7000000));
    assert(lux == 10.0);

    sabg_lux_average_clear(&average);
    lux = sabg_lux_average_observe(&average, 3.0, UINT64_C(8000000));
    assert(lux == 3.0);
    sabg_lux_average_reset(&average, 7.0, UINT64_C(9000000));
    lux = sabg_lux_average_observe(&average, 7.0, UINT64_C(10000000));
    assert(lux == 7.0);

    sabg_lux_average_reset(&average, 2.0, UINT64_C(11000000));
    lux = sabg_lux_average_observe(&average, 20.0, UINT64_C(11000000));
    assert(lux == 2.0);

    sabg_lux_average_init(&average, UINT64_C(5000000));
    for (uint64_t step = 0U; step <= 40U; step++) {
        uint64_t now_usec = step * UINT64_C(250000);
        double raw_lux = ((step / 8U) % 2U) == 0U ? 0.0 : 1.0;

        lux = sabg_lux_average_observe(&average, raw_lux, now_usec);
        if (step >= 20U) {
            assert(lux >= 0.2);
            assert(lux <= 0.8);
        }
    }

    sabg_lux_average_init(&average, UINT64_C(30000000));
    for (uint64_t step = 0U; step < 400U; step++) {
        lux = sabg_lux_average_observe(
            &average,
            (double)(step % 4U),
            step * UINT64_C(100000)
        );
        assert(isfinite(lux));
        assert(average.count <= SABG_LUX_AVERAGE_MAX_SAMPLES);
    }

    return 0;
}
