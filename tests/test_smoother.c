// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/smoother.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

int main(void)
{
    SabgSmoother smoother;

    sabg_smoother_init(&smoother, 50, 40, 60);
    assert(!sabg_smoother_active(&smoother));
    assert(!sabg_smoother_set_target(&smoother, 50));

    assert(sabg_smoother_set_target(&smoother, 53));
    assert(sabg_smoother_active(&smoother));
    assert(sabg_smoother_next_delay_usec(&smoother) == UINT64_C(40000));
    assert(sabg_smoother_advance(&smoother) == 51);
    assert(sabg_smoother_advance(&smoother) == 52);
    assert(sabg_smoother_advance(&smoother) == 53);
    assert(!sabg_smoother_active(&smoother));

    assert(sabg_smoother_set_target(&smoother, 49));
    assert(sabg_smoother_next_delay_usec(&smoother) == UINT64_C(60000));
    assert(sabg_smoother_advance(&smoother) == 52);

    assert(sabg_smoother_set_target(&smoother, 1000));
    assert(smoother.target == 100);
    assert(sabg_smoother_set_target(&smoother, -10));
    assert(smoother.target == 0);

    sabg_smoother_init(&smoother, -20, 0, 0);
    assert(smoother.current == 0);
    assert(smoother.brighten_step_ms == 1);
    assert(smoother.dim_step_ms == 1);
    assert(sabg_smoother_advance(&smoother) == 0);

    sabg_smoother_init(&smoother, 120, 5, 7);
    assert(smoother.current == 100);
    assert(!sabg_smoother_set_target(&smoother, 101));

    return 0;
}
