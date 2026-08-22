// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/trajectory.h"

#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdint.h>

static void test_capped_step_is_smooth_and_finishes(void)
{
    SabgTrajectory trajectory;
    uint64_t start = UINT64_C(1000000);
    double previous = 30.0;
    unsigned int frame;

    sabg_trajectory_init(&trajectory, 30.0, 40, 60, 250, 0, 100, start);
    sabg_trajectory_set_target(&trajectory, 80.0, start);
    assert(sabg_trajectory_active(&trajectory));
    assert(trajectory.deadline_usec == start + UINT64_C(250000));
    for (frame = 1; frame <= 15; frame++) {
        double current = sabg_trajectory_advance(
            &trajectory,
            start + (uint64_t)frame * UINT64_C(250000) / UINT64_C(15)
        );
        assert(current >= previous);
        assert(current <= 80.0);
        previous = current;
    }
    assert(trajectory.position == 80.0);
    assert(!sabg_trajectory_active(&trajectory));
}

static void test_retarget_preserves_motion(void)
{
    SabgTrajectory trajectory;
    uint64_t start = UINT64_C(2000000);
    double position;
    double velocity;

    sabg_trajectory_init(&trajectory, 20.0, 40, 60, 250, 0, 100, start);
    sabg_trajectory_set_target(&trajectory, 80.0, start);
    position = sabg_trajectory_advance(&trajectory, start + UINT64_C(80000));
    velocity = trajectory.velocity;
    assert(position > 20.0);
    assert(velocity > 0.0);

    sabg_trajectory_set_target(&trajectory, 70.0, start + UINT64_C(80000));
    assert(fabs(trajectory.velocity - velocity) < 0.0001);
    assert(sabg_trajectory_advance(&trajectory, start + UINT64_C(100000)) > position);

    sabg_trajectory_reset(&trajectory, 35.0, start + UINT64_C(110000));
    assert(trajectory.position == 35.0);
    assert(trajectory.velocity == 0.0);
    assert(!sabg_trajectory_active(&trajectory));
}

int main(void)
{
    test_capped_step_is_smooth_and_finishes();
    test_retarget_preserves_motion();
    return 0;
}

