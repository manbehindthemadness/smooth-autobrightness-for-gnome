// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/write_tracker.h"

#include <assert.h>
#include <stddef.h>

#define WRITE_MAXIMUM_AGE_USEC UINT64_C(2000000)
#define QUANTIZED_FEEDBACK_MAXIMUM_AGE_USEC UINT64_C(500000)

static void remove_entry(SabgWriteTracker *tracker, unsigned int index)
{
    unsigned int position;

    for (position = index + 1U; position < tracker->count; position++)
        tracker->entries[position - 1U] = tracker->entries[position];
    tracker->count--;
}

static void expire_old_entries(SabgWriteTracker *tracker, uint64_t now_usec)
{
    unsigned int index = 0;

    while (index < tracker->count) {
        uint64_t written_usec = tracker->entries[index].written_usec;

        if (now_usec >= written_usec
            && now_usec - written_usec > WRITE_MAXIMUM_AGE_USEC) {
            remove_entry(tracker, index);
        } else {
            index++;
        }
    }
}

void sabg_write_tracker_record(
    SabgWriteTracker *tracker,
    int percentage,
    uint64_t now_usec
)
{
    assert(tracker != NULL);

    expire_old_entries(tracker, now_usec);
    if (tracker->count == SABG_WRITE_TRACKER_CAPACITY)
        remove_entry(tracker, 0);
    tracker->entries[tracker->count] = (SabgTrackedWrite){
        .percentage = percentage,
        .written_usec = now_usec,
    };
    tracker->count++;
}

bool sabg_write_tracker_consume(
    SabgWriteTracker *tracker,
    int percentage,
    uint64_t now_usec
)
{
    unsigned int index;

    assert(tracker != NULL);

    expire_old_entries(tracker, now_usec);
    for (index = 0; index < tracker->count; index++) {
        if (tracker->entries[index].percentage == percentage) {
            remove_entry(tracker, index);
            return true;
        }
    }

    for (index = 0; index < tracker->count; index++) {
        int difference = tracker->entries[index].percentage - percentage;
        uint64_t written_usec = tracker->entries[index].written_usec;

        if ((difference == 1 || difference == -1)
            && now_usec >= written_usec
            && now_usec - written_usec <= QUANTIZED_FEEDBACK_MAXIMUM_AGE_USEC) {
            remove_entry(tracker, index);
            return true;
        }
    }
    return false;
}

void sabg_write_tracker_clear(SabgWriteTracker *tracker)
{
    assert(tracker != NULL);
    tracker->count = 0;
}
