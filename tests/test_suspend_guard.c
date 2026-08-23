// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/suspend_guard.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

int main(void)
{
    SabgSuspendGuard guard;
    SabgSuspendTransition transition;
    bool first = false;

    sabg_suspend_guard_init(&guard, false, 47, UINT64_C(2000000));
    assert(sabg_suspend_guard_accept_sample(&guard, 10, &first));
    assert(!first);

    transition = sabg_suspend_guard_set_lid(&guard, true, 47, 100);
    assert(transition.entered);
    assert(!transition.resumed);
    assert(guard.protected_brightness == 47);
    assert(!sabg_suspend_guard_accept_sample(&guard, 200, &first));

    transition = sabg_suspend_guard_set_sleep(&guard, true, 2, 300);
    assert(!transition.entered && !transition.resumed);
    transition = sabg_suspend_guard_set_sleep(&guard, false, 2, 400);
    assert(!transition.entered && !transition.resumed);
    transition = sabg_suspend_guard_set_lid(&guard, false, 2, 500);
    assert(!transition.entered && transition.resumed);
    assert(guard.protected_brightness == 47);
    assert(!sabg_suspend_guard_accept_sample(&guard, UINT64_C(2000499), &first));
    assert(sabg_suspend_guard_blocked(&guard));
    assert(sabg_suspend_guard_accept_sample(&guard, UINT64_C(2000500), &first));
    assert(first);
    assert(!sabg_suspend_guard_blocked(&guard));
    assert(sabg_suspend_guard_accept_sample(&guard, UINT64_C(2000600), &first));
    assert(!first);

    transition = sabg_suspend_guard_set_sleep(&guard, true, 61, UINT64_C(3000000));
    assert(transition.entered);
    assert(guard.protected_brightness == 61);
    transition = sabg_suspend_guard_set_sleep(&guard, false, 2, UINT64_C(4000000));
    assert(transition.resumed);
    assert(!sabg_suspend_guard_accept_sample(&guard, UINT64_C(5000000), &first));
    assert(sabg_suspend_guard_accept_sample(&guard, UINT64_C(6000000), &first));
    assert(first);

    return 0;
}
