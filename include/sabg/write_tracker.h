// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SABG_WRITE_TRACKER_CAPACITY 64U

typedef struct {
    int percentage;
    uint64_t written_usec;
} SabgTrackedWrite;

typedef struct {
    SabgTrackedWrite entries[SABG_WRITE_TRACKER_CAPACITY];
    unsigned int count;
} SabgWriteTracker;

void sabg_write_tracker_record(
    SabgWriteTracker *tracker,
    int percentage,
    uint64_t now_usec
);

bool sabg_write_tracker_consume(
    SabgWriteTracker *tracker,
    int percentage,
    uint64_t now_usec
);

void sabg_write_tracker_clear(SabgWriteTracker *tracker);
