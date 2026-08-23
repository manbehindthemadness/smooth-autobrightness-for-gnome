// SPDX-License-Identifier: GPL-3.0-or-later
#include "sabg/user_brightness.h"

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/sabg-user-brightness-XXXXXX";
    SabgUserBrightness brightness = {.display = 73, .keyboard = 41};
    SabgUserBrightness loaded = {0};
    int fd;

    fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(sabg_user_brightness_save(path, &brightness) == 0);
    assert(sabg_user_brightness_load(path, &loaded) == 0);
    assert(loaded.display == 73);
    assert(loaded.keyboard == 41);

    brightness.display = 101;
    assert(sabg_user_brightness_save(path, &brightness) < 0);
    assert(unlink(path) == 0);
    assert(sabg_user_brightness_load(path, &loaded) < 0);
    return EXIT_SUCCESS;
}
