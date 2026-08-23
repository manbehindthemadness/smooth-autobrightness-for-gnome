// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool lid_closed;
    bool preparing_sleep;
    bool resume_sample_pending;
    int protected_brightness;
    uint64_t resume_after_usec;
    uint64_t settle_usec;
} SabgSuspendGuard;

typedef struct {
    bool entered;
    bool resumed;
} SabgSuspendTransition;

void sabg_suspend_guard_init(
    SabgSuspendGuard *guard,
    bool lid_closed,
    int current_brightness,
    uint64_t settle_usec
);

SabgSuspendTransition sabg_suspend_guard_set_lid(
    SabgSuspendGuard *guard,
    bool closed,
    int current_brightness,
    uint64_t now_usec
);

SabgSuspendTransition sabg_suspend_guard_set_sleep(
    SabgSuspendGuard *guard,
    bool preparing,
    int current_brightness,
    uint64_t now_usec
);

bool sabg_suspend_guard_blocked(const SabgSuspendGuard *guard);
bool sabg_suspend_guard_accept_sample(
    SabgSuspendGuard *guard,
    uint64_t now_usec,
    bool *first_after_resume
);
