// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <systemd/sd-event.h>

typedef struct {
    int fd;
    uint64_t interval_usec;
    sd_event_source *timer;
    char path[512];
} SabgAppleAlsKeepalive;

int sabg_apple_als_keepalive_start(
    SabgAppleAlsKeepalive *keepalive,
    sd_event *event,
    const char *requested_path,
    unsigned int interval_ms
);

void sabg_apple_als_keepalive_destroy(SabgAppleAlsKeepalive *keepalive);
