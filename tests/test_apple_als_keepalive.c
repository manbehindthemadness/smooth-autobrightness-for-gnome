// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_als_keepalive.h"

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <systemd/sd-event.h>
#include <unistd.h>

static void write_reading(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ssize_t written;

    assert(fd >= 0);
    written = write(fd, "42\n", 3U);
    assert(written == 3);
    assert(close(fd) == 0);
}

int main(void)
{
    char path[] = "/tmp/sabg-apple-als-test-XXXXXX";
    SabgAppleAlsKeepalive keepalive = {0};
    sd_event *event = NULL;
    int fd;

    fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "42\n", 3U) == 3);
    assert(close(fd) == 0);

    assert(sd_event_new(&event) >= 0);
    assert(sabg_apple_als_keepalive_start(&keepalive, event, path, 10U) == 0);

    assert(sabg_apple_als_keepalive_set_enabled(&keepalive, false) == 0);
    assert(unlink(path) == 0);
    assert(sd_event_run(event, UINT64_C(30000)) == 0);
    assert(keepalive.failure_count == 0U);
    assert(sabg_apple_als_keepalive_set_enabled(&keepalive, true) == 0);
    assert(sd_event_run(event, UINT64_C(1000000)) > 0);
    assert(keepalive.failure_count == 1U);

    write_reading(path);
    assert(sd_event_run(event, UINT64_C(1000000)) > 0);
    assert(keepalive.failure_count == 0U);

    sabg_apple_als_keepalive_destroy(&keepalive);
    event = sd_event_unref(event);
    assert(event == NULL);
    assert(unlink(path) == 0);
    return EXIT_SUCCESS;
}
