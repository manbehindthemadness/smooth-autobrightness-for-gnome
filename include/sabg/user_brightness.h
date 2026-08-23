// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

typedef struct {
    int display;
    int keyboard;
} SabgUserBrightness;

int sabg_user_brightness_load(const char *path, SabgUserBrightness *brightness);
int sabg_user_brightness_save(const char *path, const SabgUserBrightness *brightness);
