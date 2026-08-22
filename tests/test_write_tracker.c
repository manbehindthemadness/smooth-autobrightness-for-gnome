// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/write_tracker.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

int main(void)
{
    SabgWriteTracker tracker = {0};
    uint64_t now = UINT64_C(1000000);
    unsigned int index;

    sabg_write_tracker_record(&tracker, 30, now);
    sabg_write_tracker_record(&tracker, 34, now + 1U);
    sabg_write_tracker_record(&tracker, 37, now + 2U);
    assert(sabg_write_tracker_consume(&tracker, 34, now + 3U));
    assert(sabg_write_tracker_consume(&tracker, 30, now + 4U));
    assert(!sabg_write_tracker_consume(&tracker, 35, now + 5U));
    assert(sabg_write_tracker_consume(&tracker, 37, now + 6U));
    assert(tracker.count == 0U);

    sabg_write_tracker_record(&tracker, 41, now);
    assert(sabg_write_tracker_consume(&tracker, 40, now + UINT64_C(100000)));
    sabg_write_tracker_record(&tracker, 41, now);
    assert(!sabg_write_tracker_consume(&tracker, 40, now + UINT64_C(500001)));
    sabg_write_tracker_clear(&tracker);

    sabg_write_tracker_record(&tracker, 50, now);
    assert(!sabg_write_tracker_consume(&tracker, 50, now + UINT64_C(2000001)));

    for (index = 0; index < SABG_WRITE_TRACKER_CAPACITY + 5U; index++)
        sabg_write_tracker_record(&tracker, (int)index, now + index);
    assert(tracker.count == SABG_WRITE_TRACKER_CAPACITY);
    assert(!sabg_write_tracker_consume(&tracker, 0, now + 100U));
    assert(sabg_write_tracker_consume(&tracker, 5, now + 100U));

    sabg_write_tracker_clear(&tracker);
    assert(tracker.count == 0U);
    return 0;
}
