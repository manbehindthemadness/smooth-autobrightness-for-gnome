// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/keyboard_model.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>

int main(void)
{
    SabgKeyboardModel model;
    int target = -1;

    sabg_keyboard_model_init(&model, 100.0, 40, 1.0, UINT64_C(1000000));
    assert(!model.suspended_by_user);
    assert(sabg_keyboard_model_observe(&model, 200.0, UINT64_C(6000000), &target));
    assert(target <= 3);

    /* An automatic target of zero never suspends the controller. */
    assert(!model.suspended_by_user);
    assert(sabg_keyboard_model_observe(&model, 25.0, UINT64_C(12000000), &target));
    assert(target > 0);

    /* Only the manual-change path gives zero its opt-out meaning. */
    sabg_keyboard_model_manual_change(&model, 25.0, 0, UINT64_C(13000000));
    assert(model.suspended_by_user);
    assert(!sabg_keyboard_model_observe(&model, 1.0, UINT64_C(14000000), &target));

    sabg_keyboard_model_manual_change(&model, 25.0, 35, UINT64_C(15000000));
    assert(!model.suspended_by_user);
    assert(sabg_keyboard_model_observe(&model, 25.0, UINT64_C(16000000), &target));
    assert(target == 35);

    /* Starting at zero is not treated as a user suspension. */
    sabg_keyboard_model_init(&model, 200.0, 0, 1.0, UINT64_C(20000000));
    assert(!model.suspended_by_user);
    assert(sabg_keyboard_model_observe(&model, 20.0, UINT64_C(25000000), &target));
    assert(target > 0);

    return 0;
}

