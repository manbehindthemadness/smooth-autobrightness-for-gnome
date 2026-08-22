// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int current;
    int target;
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
} SabgSmoother;

void sabg_smoother_init(
    SabgSmoother *smoother,
    int current,
    unsigned int brighten_step_ms,
    unsigned int dim_step_ms
);

bool sabg_smoother_set_target(SabgSmoother *smoother, int target);
bool sabg_smoother_active(const SabgSmoother *smoother);
int sabg_smoother_advance(SabgSmoother *smoother);
uint64_t sabg_smoother_next_delay_usec(const SabgSmoother *smoother);
