// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/display_response.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_PROFILE_BYTES (1024L * 1024L)

static double clamp(double value, double minimum, double maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

void sabg_display_response_init(SabgDisplayResponse *response)
{
    int index;

    for (index = 0; index < SABG_DISPLAY_RESPONSE_POINTS; index++)
        response->coordinate[index] = (double)index;
    response->calibrated = false;
}

int sabg_display_response_load(SabgDisplayResponse *response, const char *path)
{
    static const char key[] = "\"command_coordinates\"";
    SabgDisplayResponse parsed;
    char *contents = NULL;
    char *cursor;
    char *end;
    long length;
    int index;
    FILE *stream;
    int result = -EINVAL;

    stream = fopen(path, "re");
    if (stream == NULL)
        return -errno;
    if (fseek(stream, 0, SEEK_END) < 0)
        goto out;
    length = ftell(stream);
    if (length <= 0 || length > MAXIMUM_PROFILE_BYTES)
        goto out;
    if (fseek(stream, 0, SEEK_SET) < 0)
        goto out;
    contents = malloc((size_t)length + 1U);
    if (contents == NULL) {
        result = -ENOMEM;
        goto out;
    }
    if (fread(contents, 1U, (size_t)length, stream) != (size_t)length) {
        result = ferror(stream) ? -EIO : -EINVAL;
        goto out;
    }
    contents[length] = '\0';
    cursor = strstr(contents, key);
    if (cursor == NULL)
        goto out;
    cursor = strchr(cursor + sizeof(key) - 1U, '[');
    if (cursor == NULL)
        goto out;
    cursor++;

    sabg_display_response_init(&parsed);
    for (index = 0; index < SABG_DISPLAY_RESPONSE_POINTS; index++) {
        errno = 0;
        parsed.coordinate[index] = strtod(cursor, &end);
        if (errno != 0 || end == cursor || !isfinite(parsed.coordinate[index]))
            goto out;
        if (index > 0) {
            double step = parsed.coordinate[index] - parsed.coordinate[index - 1];

            if (step < 0.049999 || step > 1.000001)
                goto out;
        } else if (fabs(parsed.coordinate[index]) > 0.000001) {
            goto out;
        }
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            cursor++;
        if (index + 1 < SABG_DISPLAY_RESPONSE_POINTS) {
            if (*cursor != ',')
                goto out;
            cursor++;
        } else if (*cursor != ']') {
            goto out;
        }
    }
    parsed.calibrated = true;
    *response = parsed;
    result = 0;

out:
    free(contents);
    fclose(stream);
    return result;
}

double sabg_display_response_to_coordinate(
    const SabgDisplayResponse *response,
    double brightness
)
{
    int lower;
    double fraction;

    brightness = clamp(brightness, 0.0, 100.0);
    lower = (int)floor(brightness);
    if (lower >= 100)
        return response->coordinate[100];
    fraction = brightness - (double)lower;
    return response->coordinate[lower]
        + fraction * (response->coordinate[lower + 1] - response->coordinate[lower]);
}

double sabg_display_response_to_brightness(
    const SabgDisplayResponse *response,
    double coordinate
)
{
    int lower;
    double width;

    coordinate = clamp(coordinate, response->coordinate[0], response->coordinate[100]);
    for (lower = 0; lower < 100; lower++) {
        if (coordinate <= response->coordinate[lower + 1])
            break;
    }
    if (lower >= 100)
        return 100.0;
    width = response->coordinate[lower + 1] - response->coordinate[lower];
    return (double)lower + (coordinate - response->coordinate[lower]) / width;
}

double sabg_display_response_velocity_scale(
    const SabgDisplayResponse *response,
    double brightness
)
{
    int lower = (int)floor(clamp(brightness, 0.0, 99.999999));

    return response->coordinate[lower + 1] - response->coordinate[lower];
}
