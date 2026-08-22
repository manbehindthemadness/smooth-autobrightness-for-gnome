// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int current;
    int target;
    int start;
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
    unsigned int maximum_transition_ms;
    unsigned int frame_count;
    unsigned int frame_index;
    uint64_t start_usec;
    uint64_t duration_usec;
} SabgSmoother;

void sabg_smoother_init(
    SabgSmoother *smoother,
    int current,
    unsigned int brighten_step_ms,
    unsigned int dim_step_ms,
    unsigned int maximum_transition_ms
);

bool sabg_smoother_set_target(SabgSmoother *smoother, int target, uint64_t now_usec);
bool sabg_smoother_active(const SabgSmoother *smoother);
int sabg_smoother_advance(SabgSmoother *smoother, uint64_t now_usec);
uint64_t sabg_smoother_next_wakeup_usec(const SabgSmoother *smoother);
void sabg_smoother_reset(SabgSmoother *smoother, int current);
