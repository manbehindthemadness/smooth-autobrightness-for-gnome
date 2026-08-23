// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/suspend_guard.h"

#include <assert.h>
#include <stddef.h>

static bool physically_blocked(const SabgSuspendGuard *guard)
{
    return guard->lid_closed || guard->preparing_sleep;
}

static SabgSuspendTransition update(
    SabgSuspendGuard *guard,
    bool was_blocked,
    int current_brightness,
    uint64_t now_usec
)
{
    SabgSuspendTransition transition = {0};
    bool blocked = physically_blocked(guard);

    if (!was_blocked && blocked) {
        guard->protected_brightness = current_brightness;
        guard->resume_after_usec = 0;
        guard->resume_sample_pending = true;
        transition.entered = true;
    } else if (was_blocked && !blocked) {
        guard->resume_after_usec = now_usec + guard->settle_usec;
        transition.resumed = true;
    }
    return transition;
}

void sabg_suspend_guard_init(
    SabgSuspendGuard *guard,
    bool lid_closed,
    int current_brightness,
    uint64_t settle_usec
)
{
    assert(guard != NULL);
    guard->lid_closed = lid_closed;
    guard->preparing_sleep = false;
    guard->resume_sample_pending = lid_closed;
    guard->protected_brightness = current_brightness;
    guard->resume_after_usec = 0;
    guard->settle_usec = settle_usec;
}

SabgSuspendTransition sabg_suspend_guard_set_lid(
    SabgSuspendGuard *guard,
    bool closed,
    int current_brightness,
    uint64_t now_usec
)
{
    bool was_blocked;

    assert(guard != NULL);
    was_blocked = physically_blocked(guard);
    guard->lid_closed = closed;
    return update(guard, was_blocked, current_brightness, now_usec);
}

SabgSuspendTransition sabg_suspend_guard_set_sleep(
    SabgSuspendGuard *guard,
    bool preparing,
    int current_brightness,
    uint64_t now_usec
)
{
    bool was_blocked;

    assert(guard != NULL);
    was_blocked = physically_blocked(guard);
    guard->preparing_sleep = preparing;
    return update(guard, was_blocked, current_brightness, now_usec);
}

bool sabg_suspend_guard_blocked(const SabgSuspendGuard *guard)
{
    assert(guard != NULL);
    return physically_blocked(guard) || guard->resume_sample_pending;
}

bool sabg_suspend_guard_accept_sample(
    SabgSuspendGuard *guard,
    uint64_t now_usec,
    bool *first_after_resume
)
{
    assert(guard != NULL);
    assert(first_after_resume != NULL);
    *first_after_resume = false;
    if (physically_blocked(guard)
        || (guard->resume_sample_pending && now_usec < guard->resume_after_usec)) {
        return false;
    }
    if (guard->resume_sample_pending) {
        guard->resume_sample_pending = false;
        *first_after_resume = true;
    }
    return true;
}
