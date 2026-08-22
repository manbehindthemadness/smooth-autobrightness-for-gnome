// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "sabg/display_response.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    SabgDisplayResponse response;
    char path[] = "/tmp/sabg-response-XXXXXX";
    FILE *stream;
    int descriptor;
    int index;

    sabg_display_response_init(&response);
    assert(!response.calibrated);
    assert(fabs(sabg_display_response_to_coordinate(&response, 42.5) - 42.5) < 1e-9);
    assert(fabs(sabg_display_response_to_brightness(&response, 42.5) - 42.5) < 1e-9);

    response.coordinate[1] = 0.2;
    response.coordinate[2] = 0.4;
    assert(fabs(sabg_display_response_to_coordinate(&response, 1.5) - 0.3) < 1e-9);
    assert(fabs(sabg_display_response_to_brightness(&response, 0.3) - 1.5) < 1e-9);
    assert(fabs(sabg_display_response_velocity_scale(&response, 1.0) - 0.2) < 1e-9);

    descriptor = mkstemp(path);
    assert(descriptor >= 0);
    stream = fdopen(descriptor, "w");
    assert(stream != NULL);
    fputs("{\"command_coordinates\":[", stream);
    for (index = 0; index < SABG_DISPLAY_RESPONSE_POINTS; index++)
        fprintf(stream, "%s%.2f", index == 0 ? "" : ",", (double)index * 0.2);
    fputs("]}\n", stream);
    assert(fclose(stream) == 0);
    sabg_display_response_init(&response);
    assert(sabg_display_response_load(&response, path) == 0);
    assert(response.calibrated);
    assert(fabs(sabg_display_response_to_coordinate(&response, 50.0) - 10.0) < 1e-9);
    assert(unlink(path) == 0);
    return 0;
}
