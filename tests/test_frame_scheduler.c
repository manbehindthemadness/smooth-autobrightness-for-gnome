// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/frame_scheduler.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

int main(void)
{
    assert(sabg_frame_scheduler_rate(0.0, 0.0, 0, 10, 60, 0.5) == 10);
    assert(sabg_frame_scheduler_rate(4.0, 0.0, 0, 10, 60, 0.5) == 10);
    assert(sabg_frame_scheduler_rate(10.0, 0.0, 0, 10, 60, 0.5) == 20);
    assert(sabg_frame_scheduler_rate(100.0, 0.0, 0, 10, 60, 0.5) == 60);
    assert(sabg_frame_scheduler_rate(0.0, 5.0, UINT64_C(250000), 10, 60, 0.5) == 40);
    assert(sabg_frame_scheduler_rate(0.0, 10.0, UINT64_C(250000), 10, 60, 0.5) == 60);
    assert(sabg_frame_scheduler_rate(0.0, 1.0, 0, 10, 60, 0.5) == 60);
    assert(sabg_frame_scheduler_rate(0.0, 0.0, 0, 2, 60, 0.2) == 2);
    assert(sabg_frame_scheduler_rate(0.4, 0.0, 0, 2, 60, 0.2) == 2);
    assert(sabg_frame_scheduler_rate(5.0, 0.0, 0, 2, 60, 0.2) == 25);
    assert(sabg_frame_scheduler_rate(8.0, 0.0, 0, 2, 60, 0.2) == 40);
    assert(sabg_frame_scheduler_rate(12.0, 0.0, 0, 2, 60, 0.2) == 60);
    return 0;
}
