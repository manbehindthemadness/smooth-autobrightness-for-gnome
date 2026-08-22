// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/ambient_model.h"

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>

int main(void)
{
    SabgAmbientModel model;
    int target;

    sabg_ambient_model_init(&model, 100.0, 50, 1.0, 2, 100, UINT64_C(1000000));
    assert(model.normalization_lux == 200.0);

    target = sabg_ambient_model_observe(&model, 200.0, UINT64_C(2000000));
    assert(target > 50);
    assert(target < 100);

    target = sabg_ambient_model_observe(&model, 200.0, UINT64_C(5000000));
    assert(target >= 97);
    assert(target <= 100);

    sabg_ambient_model_recalibrate(&model, 200.0, 40, UINT64_C(6000000));
    assert(model.normalization_lux == 500.0);
    target = sabg_ambient_model_observe(&model, 200.0, UINT64_C(7000000));
    assert(target == 40);

    target = sabg_ambient_model_observe(&model, 0.0, UINT64_C(12000000));
    assert(target >= 2);

    sabg_ambient_model_init(&model, 0.0, 0, 0.0, 5, 80, UINT64_C(20));
    assert(model.normalization_lux == 1.0);
    assert(model.filtered_percentage == 5.0);
    target = sabg_ambient_model_observe(&model, 10000.0, UINT64_C(20));
    assert(target == 5);
    target = sabg_ambient_model_observe(&model, 10000.0, UINT64_C(1000020));
    assert(target == 80);

    sabg_ambient_model_recalibrate(&model, 10.0, -50, UINT64_C(2000020));
    assert(model.filtered_percentage == 5.0);
    sabg_ambient_model_recalibrate(&model, 10.0, 500, UINT64_C(3000020));
    assert(model.filtered_percentage == 80.0);

    sabg_ambient_model_init(&model, 100.0, 20, 3.0, 2, 100, UINT64_C(0));
    sabg_ambient_model_set_dimming_time_constant(&model, 0.5);
    assert(fabs(
        sabg_ambient_model_advance(&model, 0.0, UINT64_C(500000))
        - (20.0 + (2.0 - 20.0) * (1.0 - exp(-1.0)))
    ) < 0.001);
    sabg_ambient_model_set_dimming_finish_distance(&model, 4.0);
    model.filtered_percentage = 6.1;
    model.last_update_usec = UINT64_C(500000);
    assert(sabg_ambient_model_advance(&model, 0.0, UINT64_C(600000)) == 2.0);
    model.filtered_percentage = 20.0;
    model.last_update_usec = UINT64_C(600000);
    assert(fabs(
        sabg_ambient_model_advance(&model, 500.0, UINT64_C(1100000))
        - (20.0 + (100.0 - 20.0) * (1.0 - exp(-0.5 / 3.0)))
    ) < 0.001);
    sabg_ambient_model_set_activity_threshold(&model, 2.0);
    model.filtered_percentage = 99.0;
    assert(!sabg_ambient_model_active(&model));
    model.filtered_percentage = 97.0;
    assert(sabg_ambient_model_active(&model));

    sabg_ambient_model_init(&model, 100.0, 2, 1.6, 2, 100, UINT64_C(0));
    sabg_ambient_model_set_large_change_response(&model, 12.0, 0.224, 4.0);
    assert(sabg_ambient_model_advance(&model, 500.0, UINT64_C(100000)) < 3.0);
    assert(!model.large_change_active);
    assert(sabg_ambient_model_advance(&model, 5000.0, UINT64_C(200000)) > 30.0);
    assert(model.large_change_active);
    assert(sabg_ambient_model_advance(&model, 5000.0, UINT64_C(1000000)) == 100.0);
    assert(!model.large_change_active);

    return 0;
}
