// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/user_brightness.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int valid(const SabgUserBrightness *brightness)
{
    return brightness->display >= 0 && brightness->display <= 100
        && brightness->keyboard >= 0 && brightness->keyboard <= 100;
}

int sabg_user_brightness_load(const char *path, SabgUserBrightness *brightness)
{
    SabgUserBrightness loaded;
    int character;
    FILE *stream;
    int matched;

    if (path == NULL || brightness == NULL)
        return -EINVAL;
    stream = fopen(path, "re");
    if (stream == NULL)
        return -errno;
    matched = fscanf(stream, "display=%d\nkeyboard=%d", &loaded.display, &loaded.keyboard);
    do {
        character = fgetc(stream);
    } while (character == ' ' || character == '\t' || character == '\r' || character == '\n');
    if (fclose(stream) != 0)
        return -errno;
    if (matched != 2 || character != EOF || !valid(&loaded))
        return -EINVAL;
    *brightness = loaded;
    return 0;
}

int sabg_user_brightness_save(const char *path, const SabgUserBrightness *brightness)
{
    char temporary[PATH_MAX];
    FILE *stream;
    int written;
    int result = 0;

    if (path == NULL || brightness == NULL || !valid(brightness))
        return -EINVAL;
    written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(temporary))
        return -ENAMETOOLONG;

    stream = fopen(temporary, "we");
    if (stream == NULL)
        return -errno;
    if (fprintf(
        stream,
        "display=%d\nkeyboard=%d\n",
        brightness->display,
        brightness->keyboard
    ) < 0 || fflush(stream) != 0 || fsync(fileno(stream)) != 0) {
        result = -errno;
    }
    if (fclose(stream) != 0 && result == 0)
        result = -errno;
    if (result == 0 && rename(temporary, path) != 0)
        result = -errno;
    if (result < 0)
        (void)unlink(temporary);
    return result;
}
