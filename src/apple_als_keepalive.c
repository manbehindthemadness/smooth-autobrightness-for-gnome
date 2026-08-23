// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_als_keepalive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define IIO_ROOT "/sys/bus/iio/devices"
#define ERROR_LOG_INTERVAL_USEC (UINT64_C(60) * UINT64_C(1000000))

static int read_text_file(const char *path, char *buffer, size_t size)
{
    int fd;
    ssize_t count;

    if (size < 2U)
        return -EINVAL;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    count = read(fd, buffer, size - 1U);
    if (count < 0) {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }

    close(fd);
    buffer[count] = '\0';
    while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r')) {
        buffer[count - 1] = '\0';
        count--;
    }
    return 0;
}
static int discover_apple_als(char *result, size_t result_size)
{
    DIR *directory;
    struct dirent *entry;
    char name_path[512];
    char name[64];
    int found = -ENOENT;

    directory = opendir(IIO_ROOT);
    if (directory == NULL)
        return -errno;

    while ((entry = readdir(directory)) != NULL) {
        int written;

        if (strncmp(entry->d_name, "iio:device", 10U) != 0)
            continue;

        written = snprintf(
            name_path,
            sizeof(name_path),
            "%s/%s/name",
            IIO_ROOT,
            entry->d_name
        );
        if (written < 0 || (size_t)written >= sizeof(name_path))
            continue;
        if (read_text_file(name_path, name, sizeof(name)) < 0)
            continue;
        if (strcmp(name, "als") != 0)
            continue;

        written = snprintf(
            result,
            result_size,
            "%s/%s/in_illuminance_input",
            IIO_ROOT,
            entry->d_name
        );
        if (written < 0 || (size_t)written >= result_size) {
            found = -ENAMETOOLONG;
            break;
        }
        if (access(result, R_OK) == 0)
            found = 0;
        break;
    }

    closedir(directory);
    return found;
}

static int read_sensor(const char *path)
{
    char buffer[64];
    int fd;
    ssize_t count;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }

    close(fd);
    return 0;
}

static int refresh_sensor(SabgAppleAlsKeepalive *keepalive)
{
    char discovered_path[sizeof(keepalive->path)];
    int result;

    result = read_sensor(keepalive->path);
    if (result >= 0 || keepalive->explicit_path)
        return result;

    if (discover_apple_als(discovered_path, sizeof(discovered_path)) < 0)
        return result;

    memcpy(keepalive->path, discovered_path, strlen(discovered_path) + 1U);
    return read_sensor(keepalive->path);
}

static int on_keepalive_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    SabgAppleAlsKeepalive *keepalive = userdata;
    sd_event *event = sd_event_source_get_event(source);
    uint64_t now = usec;
    int result;

    result = sd_event_now(event, CLOCK_MONOTONIC, &now);
    if (result < 0)
        return result;

    result = refresh_sensor(keepalive);
    if (result < 0) {
        keepalive->failure_count++;
        if (keepalive->next_error_log_usec == 0 || now >= keepalive->next_error_log_usec) {
            fprintf(stderr,
                "Apple ALS keepalive read failed: %s; will retry\n",
                strerror(-result));
            keepalive->next_error_log_usec = now + ERROR_LOG_INTERVAL_USEC;
        }
    } else if (keepalive->failure_count > 0) {
        fprintf(stderr,
            "Apple ALS keepalive recovered after %llu failed refresh%s: %s\n",
            (unsigned long long)keepalive->failure_count,
            keepalive->failure_count == 1 ? "" : "es",
            keepalive->path);
        keepalive->failure_count = 0;
        keepalive->next_error_log_usec = 0;
    }

    result = sd_event_source_set_time(source, now + keepalive->interval_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
}

int sabg_apple_als_keepalive_start(
    SabgAppleAlsKeepalive *keepalive,
    sd_event *event,
    const char *requested_path,
    unsigned int interval_ms
)
{
    uint64_t now;
    int result;

    if (keepalive == NULL || event == NULL)
        return -EINVAL;

    memset(keepalive, 0, sizeof(*keepalive));
    keepalive->interval_usec = (uint64_t)interval_ms * UINT64_C(1000);
    if (keepalive->interval_usec == 0)
        return -EINVAL;

    if (requested_path != NULL) {
        size_t length = strlen(requested_path);
        if (length >= sizeof(keepalive->path))
            return -ENAMETOOLONG;
        memcpy(keepalive->path, requested_path, length + 1U);
        keepalive->explicit_path = true;
    } else {
        result = discover_apple_als(keepalive->path, sizeof(keepalive->path));
        if (result < 0)
            return result;
    }

    result = refresh_sensor(keepalive);
    if (result < 0)
        goto fail;

    result = sd_event_now(event, CLOCK_MONOTONIC, &now);
    if (result < 0)
        goto fail;

    result = sd_event_add_time(
        event,
        &keepalive->timer,
        CLOCK_MONOTONIC,
        now + keepalive->interval_usec,
        keepalive->interval_usec / UINT64_C(10),
        on_keepalive_timer,
        keepalive
    );
    if (result < 0)
        goto fail;

    return 0;

fail:
    sabg_apple_als_keepalive_destroy(keepalive);
    return result;
}

int sabg_apple_als_keepalive_set_enabled(
    SabgAppleAlsKeepalive *keepalive,
    bool enabled
)
{
    sd_event *event;
    uint64_t now;
    int result;

    if (keepalive == NULL || keepalive->timer == NULL)
        return -EINVAL;
    if (!enabled)
        return sd_event_source_set_enabled(keepalive->timer, SD_EVENT_OFF);

    event = sd_event_source_get_event(keepalive->timer);
    result = sd_event_now(event, CLOCK_MONOTONIC, &now);
    if (result < 0)
        return result;
    result = sd_event_source_set_time(
        keepalive->timer,
        now + keepalive->interval_usec
    );
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(keepalive->timer, SD_EVENT_ONESHOT);
}

void sabg_apple_als_keepalive_destroy(SabgAppleAlsKeepalive *keepalive)
{
    if (keepalive == NULL)
        return;

    keepalive->timer = sd_event_source_unref(keepalive->timer);
}
