// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>

#define SABG_DISPLAY_RESPONSE_POINTS 101

typedef struct {
    double coordinate[SABG_DISPLAY_RESPONSE_POINTS];
    bool calibrated;
} SabgDisplayResponse;

void sabg_display_response_init(SabgDisplayResponse *response);
int sabg_display_response_load(SabgDisplayResponse *response, const char *path);
double sabg_display_response_to_coordinate(
    const SabgDisplayResponse *response,
    double brightness
);
double sabg_display_response_to_brightness(
    const SabgDisplayResponse *response,
    double coordinate
);
double sabg_display_response_velocity_scale(
    const SabgDisplayResponse *response,
    double brightness
);
