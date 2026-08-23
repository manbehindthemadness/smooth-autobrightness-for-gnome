// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-event.h>

typedef struct {
    uint64_t interval_usec;
    uint64_t failure_count;
    uint64_t next_error_log_usec;
    sd_event_source *timer;
    bool explicit_path;
    char path[512];
} SabgAppleAlsKeepalive;

int sabg_apple_als_keepalive_start(
    SabgAppleAlsKeepalive *keepalive,
    sd_event *event,
    const char *requested_path,
    unsigned int interval_ms
);

int sabg_apple_als_keepalive_set_enabled(
    SabgAppleAlsKeepalive *keepalive,
    bool enabled
);

void sabg_apple_als_keepalive_destroy(SabgAppleAlsKeepalive *keepalive);
