// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_als_keepalive.h"
#include "sabg/ambient_model.h"
#include "sabg/keyboard_model.h"
#include "sabg/smoother.h"
#include "sabg/write_tracker.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <time.h>

#define VERSION "0.3.0"

#define SENSOR_DESTINATION "net.hadess.SensorProxy"
#define SENSOR_PATH "/net/hadess/SensorProxy"
#define SENSOR_INTERFACE "net.hadess.SensorProxy"

#define GNOME_POWER_DESTINATION "org.gnome.SettingsDaemon.Power"
#define GNOME_POWER_PATH "/org/gnome/SettingsDaemon/Power"
#define GNOME_SCREEN_INTERFACE "org.gnome.SettingsDaemon.Power.Screen"
#define GNOME_KEYBOARD_INTERFACE "org.gnome.SettingsDaemon.Power.Keyboard"

#define UPOWER_DESTINATION "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_INTERFACE "org.freedesktop.UPower"
#define UPOWER_KEYBOARD_PATH "/org/freedesktop/UPower/KbdBacklight"
#define UPOWER_KEYBOARD_INTERFACE "org.freedesktop.UPower.KbdBacklight"

typedef enum {
    KEYBOARD_BACKEND_NONE,
    KEYBOARD_BACKEND_GNOME,
    KEYBOARD_BACKEND_UPOWER,
} KeyboardBackend;

typedef struct {
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
    unsigned int maximum_transition_ms;
    unsigned int apple_refresh_ms;
    double ambient_time_constant_seconds;
    int minimum_percentage;
    int maximum_percentage;
    bool keyboard_backlight;
    bool apple_keepalive;
    bool check_only;
    bool dry_run;
    bool verbose;
    const char *apple_als_path;
} Configuration;

typedef struct {
    Configuration configuration;
    sd_event *event;
    sd_event_source *animation_timer;
    sd_event_source *keyboard_animation_timer;
    sd_bus *system_bus;
    sd_bus *session_bus;
    sd_bus_slot *sensor_properties_slot;
    sd_bus_slot *brightness_properties_slot;
    sd_bus_slot *keyboard_brightness_slot;
    SabgAmbientModel ambient;
    SabgSmoother smoother;
    SabgWriteTracker write_tracker;
    SabgKeyboardModel keyboard_model;
    SabgSmoother keyboard_smoother;
    SabgWriteTracker keyboard_write_tracker;
    SabgAppleAlsKeepalive keepalive;
    double current_lux;
    bool light_claimed;
    bool model_ready;
    KeyboardBackend keyboard_backend;
    char keyboard_object_path[PATH_MAX];
    int keyboard_maximum_raw;
    bool keyboard_model_ready;
} Application;

static uint64_t monotonic_usec(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000)
        + (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static int parse_int(const char *text, int minimum, int maximum, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum)
        return -EINVAL;
    *value = (int)parsed;
    return 0;
}

static int parse_double(const char *text, double minimum, double maximum, double *value)
{
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)
        || parsed < minimum || parsed > maximum)
        return -EINVAL;
    *value = parsed;
    return 0;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(
        stream,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Smooth GNOME ambient-light display and keyboard brightness changes.\n"
        "\n"
        "  --brighten-step-ms N       delay per 1%% increase (default: 40)\n"
        "  --dim-step-ms N            delay per 1%% decrease (default: 60)\n"
        "  --max-transition-ms N      maximum automatic fade time (default: 250)\n"
        "  --ambient-time-constant S  target filter time constant (default: 1.6)\n"
        "  --min-brightness N         automatic floor (default: 2)\n"
        "  --max-brightness N         automatic ceiling (default: 100)\n"
        "  --no-keyboard-backlight    leave keyboard illumination unchanged\n"
        "  --apple-als-keepalive      enable optional Apple IIO refreshes\n"
        "  --apple-als-path PATH      refresh a specific IIO illuminance file\n"
        "  --apple-refresh-ms N       refresh interval (default: 500)\n"
        "  --check                    verify interfaces without changing brightness\n"
        "  --dry-run                  calculate and log without changing brightness\n"
        "  --verbose                  log observations and transitions\n"
        "  --version                  print version\n"
        "  --help                     show this help\n",
        program
    );
}

static int parse_arguments(int argc, char **argv, Configuration *configuration)
{
    enum {
        OPTION_BRIGHTEN_STEP = 1000,
        OPTION_DIM_STEP,
        OPTION_MAX_TRANSITION,
        OPTION_AMBIENT_TIME_CONSTANT,
        OPTION_MIN_BRIGHTNESS,
        OPTION_MAX_BRIGHTNESS,
        OPTION_NO_KEYBOARD_BACKLIGHT,
        OPTION_APPLE_KEEPALIVE,
        OPTION_APPLE_PATH,
        OPTION_APPLE_REFRESH,
        OPTION_CHECK,
        OPTION_DRY_RUN,
        OPTION_VERBOSE,
        OPTION_VERSION,
    };
    static const struct option options[] = {
        {"brighten-step-ms", required_argument, NULL, OPTION_BRIGHTEN_STEP},
        {"dim-step-ms", required_argument, NULL, OPTION_DIM_STEP},
        {"max-transition-ms", required_argument, NULL, OPTION_MAX_TRANSITION},
        {"ambient-time-constant", required_argument, NULL, OPTION_AMBIENT_TIME_CONSTANT},
        {"min-brightness", required_argument, NULL, OPTION_MIN_BRIGHTNESS},
        {"max-brightness", required_argument, NULL, OPTION_MAX_BRIGHTNESS},
        {"no-keyboard-backlight", no_argument, NULL, OPTION_NO_KEYBOARD_BACKLIGHT},
        {"apple-als-keepalive", no_argument, NULL, OPTION_APPLE_KEEPALIVE},
        {"apple-als-path", required_argument, NULL, OPTION_APPLE_PATH},
        {"apple-refresh-ms", required_argument, NULL, OPTION_APPLE_REFRESH},
        {"check", no_argument, NULL, OPTION_CHECK},
        {"dry-run", no_argument, NULL, OPTION_DRY_RUN},
        {"verbose", no_argument, NULL, OPTION_VERBOSE},
        {"version", no_argument, NULL, OPTION_VERSION},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int option;
    int parsed;

    *configuration = (Configuration){
        .brighten_step_ms = 40,
        .dim_step_ms = 60,
        .maximum_transition_ms = 250,
        .apple_refresh_ms = 500,
        .ambient_time_constant_seconds = 1.6,
        .minimum_percentage = 2,
        .maximum_percentage = 100,
        .keyboard_backlight = true,
    };

    while ((option = getopt_long(argc, argv, "h", options, NULL)) != -1) {
        switch (option) {
        case OPTION_BRIGHTEN_STEP:
            if (parse_int(optarg, 1, 5000, &parsed) < 0)
                return -EINVAL;
            configuration->brighten_step_ms = (unsigned int)parsed;
            break;
        case OPTION_DIM_STEP:
            if (parse_int(optarg, 1, 5000, &parsed) < 0)
                return -EINVAL;
            configuration->dim_step_ms = (unsigned int)parsed;
            break;
        case OPTION_MAX_TRANSITION:
            if (parse_int(optarg, 1, 10000, &parsed) < 0)
                return -EINVAL;
            configuration->maximum_transition_ms = (unsigned int)parsed;
            break;
        case OPTION_AMBIENT_TIME_CONSTANT:
            if (parse_double(optarg, 0.01, 300.0, &configuration->ambient_time_constant_seconds) < 0)
                return -EINVAL;
            break;
        case OPTION_MIN_BRIGHTNESS:
            if (parse_int(optarg, 0, 100, &configuration->minimum_percentage) < 0)
                return -EINVAL;
            break;
        case OPTION_MAX_BRIGHTNESS:
            if (parse_int(optarg, 0, 100, &configuration->maximum_percentage) < 0)
                return -EINVAL;
            break;
        case OPTION_NO_KEYBOARD_BACKLIGHT:
            configuration->keyboard_backlight = false;
            break;
        case OPTION_APPLE_KEEPALIVE:
            configuration->apple_keepalive = true;
            break;
        case OPTION_APPLE_PATH:
            configuration->apple_keepalive = true;
            configuration->apple_als_path = optarg;
            break;
        case OPTION_APPLE_REFRESH:
            if (parse_int(optarg, 50, 60000, &parsed) < 0)
                return -EINVAL;
            configuration->apple_refresh_ms = (unsigned int)parsed;
            break;
        case OPTION_CHECK:
            configuration->check_only = true;
            break;
        case OPTION_DRY_RUN:
            configuration->dry_run = true;
            break;
        case OPTION_VERBOSE:
            configuration->verbose = true;
            break;
        case OPTION_VERSION:
            printf("smooth-autobrightness-for-gnome %s\n", VERSION);
            exit(EXIT_SUCCESS);
        case 'h':
            print_usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        default:
            return -EINVAL;
        }
    }

    if (optind != argc || configuration->minimum_percentage > configuration->maximum_percentage)
        return -EINVAL;
    return 0;
}

static int read_changed_double(
    sd_bus_message *message,
    const char *expected_interface,
    const char *property,
    double *value
)
{
    const char *interface = NULL;
    int result;
    int found = 0;

    result = sd_bus_message_read(message, "s", &interface);
    if (result < 0)
        return result;
    if (strcmp(interface, expected_interface) != 0)
        return 0;

    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (result < 0)
        return result;
    while ((result = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *name = NULL;

        result = sd_bus_message_read(message, "s", &name);
        if (result < 0)
            return result;
        if (strcmp(name, property) == 0) {
            result = sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "d");
            if (result < 0)
                return result;
            result = sd_bus_message_read(message, "d", value);
            if (result < 0)
                return result;
            result = sd_bus_message_exit_container(message);
            if (result < 0)
                return result;
            found = 1;
        } else {
            result = sd_bus_message_skip(message, "v");
            if (result < 0)
                return result;
        }
        result = sd_bus_message_exit_container(message);
        if (result < 0)
            return result;
    }
    if (result < 0)
        return result;
    result = sd_bus_message_exit_container(message);
    return result < 0 ? result : found;
}

static int read_changed_int(
    sd_bus_message *message,
    const char *expected_interface,
    const char *property,
    int *value
)
{
    const char *interface = NULL;
    int32_t property_value = 0;
    int result;
    int found = 0;

    result = sd_bus_message_read(message, "s", &interface);
    if (result < 0)
        return result;
    if (strcmp(interface, expected_interface) != 0)
        return 0;

    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (result < 0)
        return result;
    while ((result = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *name = NULL;

        result = sd_bus_message_read(message, "s", &name);
        if (result < 0)
            return result;
        if (strcmp(name, property) == 0) {
            result = sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "i");
            if (result < 0)
                return result;
            result = sd_bus_message_read(message, "i", &property_value);
            if (result < 0)
                return result;
            result = sd_bus_message_exit_container(message);
            if (result < 0)
                return result;
            *value = (int)property_value;
            found = 1;
        } else {
            result = sd_bus_message_skip(message, "v");
            if (result < 0)
                return result;
        }
        result = sd_bus_message_exit_container(message);
        if (result < 0)
            return result;
    }
    if (result < 0)
        return result;
    result = sd_bus_message_exit_container(message);
    return result < 0 ? result : found;
}

static int schedule_animation(Application *application, uint64_t wakeup_usec);

static int on_brightness_write_reply(
    sd_bus_message *message,
    void *userdata,
    sd_bus_error *ret_error
)
{
    Application *application = userdata;
    const sd_bus_error *error;

    (void)ret_error;
    if (!sd_bus_message_is_method_error(message, NULL))
        return 0;

    error = sd_bus_message_get_error(message);
    sabg_write_tracker_clear(&application->write_tracker);
    fprintf(
        stderr,
        "Unable to set GNOME brightness: %s\n",
        error != NULL && error->message != NULL ? error->message : "unknown D-Bus error"
    );
    return 0;
}

static int write_brightness(Application *application, int percentage)
{
    sd_bus_message *message = NULL;
    int result;

    if (application->configuration.dry_run) {
        if (application->configuration.verbose)
            fprintf(stderr, "dry-run brightness: %d%%\n", percentage);
        return 0;
    }

    result = sd_bus_message_new_method_call(
        application->session_bus,
        &message,
        GNOME_POWER_DESTINATION,
        GNOME_POWER_PATH,
        "org.freedesktop.DBus.Properties",
        "Set"
    );
    if (result >= 0)
        result = sd_bus_message_append(message, "ss", GNOME_SCREEN_INTERFACE, "Brightness");
    if (result >= 0)
        result = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "i");
    if (result >= 0)
        result = sd_bus_message_append(message, "i", (int32_t)percentage);
    if (result >= 0)
        result = sd_bus_message_close_container(message);
    if (result >= 0) {
        result = sd_bus_call_async(
            application->session_bus,
            NULL,
            message,
            on_brightness_write_reply,
            application,
            0
        );
        if (result >= 0) {
            sabg_write_tracker_record(
                &application->write_tracker,
                percentage,
                monotonic_usec()
            );
        }
    }
    message = sd_bus_message_unref(message);
    if (result < 0)
        fprintf(stderr, "Unable to queue GNOME brightness: %s\n", strerror(-result));
    return result;
}

static int on_animation_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    int percentage;
    int result;

    (void)source;
    (void)usec;
    percentage = sabg_smoother_advance(&application->smoother, monotonic_usec());
    result = write_brightness(application, percentage);
    if (result < 0)
        return result;

    if (sabg_smoother_active(&application->smoother))
        return schedule_animation(
            application,
            sabg_smoother_next_wakeup_usec(&application->smoother)
        );

    if (application->configuration.verbose)
        fprintf(stderr, "ambient transition complete at %d%%\n", percentage);
    return 0;
}

static int schedule_animation(Application *application, uint64_t wakeup_usec)
{
    int result;

    if (application->animation_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->animation_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_animation_timer,
            application
        );
    }

    result = sd_event_source_set_time(application->animation_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(application->animation_timer, SD_EVENT_ONESHOT);
}

static int raw_to_percentage(int raw, int maximum)
{
    if (maximum <= 0)
        return 0;
    if (raw <= 0)
        return 0;
    if (raw >= maximum)
        return 100;
    return (int)(((int64_t)raw * INT64_C(100) + (int64_t)maximum / 2) / maximum);
}

static int percentage_to_raw(int percentage, int maximum)
{
    if (percentage <= 0 || maximum <= 0)
        return 0;
    if (percentage >= 100)
        return maximum;
    return (int)(((int64_t)percentage * maximum + INT64_C(50)) / INT64_C(100));
}

static int read_upower_keyboard_state(
    Application *application,
    const char *path,
    int *percentage
)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int32_t maximum = 0;
    int32_t current = 0;
    int result;

    result = sd_bus_call_method(
        application->system_bus,
        UPOWER_DESTINATION,
        path,
        UPOWER_KEYBOARD_INTERFACE,
        "GetMaxBrightness",
        &error,
        &reply,
        ""
    );
    if (result >= 0)
        result = sd_bus_message_read(reply, "i", &maximum);
    reply = sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (result < 0 || maximum <= 0)
        return result < 0 ? result : -ENODEV;

    result = sd_bus_call_method(
        application->system_bus,
        UPOWER_DESTINATION,
        path,
        UPOWER_KEYBOARD_INTERFACE,
        "GetBrightness",
        &error,
        &reply,
        ""
    );
    if (result >= 0)
        result = sd_bus_message_read(reply, "i", &current);
    reply = sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (result < 0)
        return result;

    application->keyboard_maximum_raw = (int)maximum;
    *percentage = raw_to_percentage((int)current, (int)maximum);
    return 0;
}

static int find_upower_keyboard(Application *application, int *percentage)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    int result;

    result = sd_bus_call_method(
        application->system_bus,
        UPOWER_DESTINATION,
        UPOWER_PATH,
        UPOWER_INTERFACE,
        "EnumerateKbdBacklights",
        &error,
        &reply,
        ""
    );
    if (result >= 0)
        result = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "o");
    if (result >= 0)
        result = sd_bus_message_read(reply, "o", &path);
    if (result > 0 && path != NULL) {
        if (strlen(path) >= sizeof(application->keyboard_object_path))
            result = -ENAMETOOLONG;
        else
            strcpy(application->keyboard_object_path, path);
    } else if (result >= 0) {
        result = -ENODEV;
    }
    reply = sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    if (result < 0)
        strcpy(application->keyboard_object_path, UPOWER_KEYBOARD_PATH);
    result = read_upower_keyboard_state(
        application,
        application->keyboard_object_path,
        percentage
    );
    return result;
}

static int discover_keyboard_backlight(Application *application, int *percentage)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int32_t value = 0;
    int result;

    if (!application->configuration.keyboard_backlight)
        return -EOPNOTSUPP;

    result = sd_bus_get_property_trivial(
        application->session_bus,
        GNOME_POWER_DESTINATION,
        GNOME_POWER_PATH,
        GNOME_KEYBOARD_INTERFACE,
        "Brightness",
        &error,
        'i',
        &value
    );
    sd_bus_error_free(&error);
    if (result >= 0 && value >= 0) {
        application->keyboard_backend = KEYBOARD_BACKEND_GNOME;
        application->keyboard_maximum_raw = 100;
        strcpy(application->keyboard_object_path, GNOME_POWER_PATH);
        *percentage = (int)value;
        return 0;
    }

    result = find_upower_keyboard(application, percentage);
    if (result >= 0)
        application->keyboard_backend = KEYBOARD_BACKEND_UPOWER;
    return result;
}

static const char *keyboard_backend_name(const Application *application)
{
    switch (application->keyboard_backend) {
    case KEYBOARD_BACKEND_GNOME:
        return "GNOME";
    case KEYBOARD_BACKEND_UPOWER:
        return "UPower";
    case KEYBOARD_BACKEND_NONE:
    default:
        return "none";
    }
}

static int on_keyboard_write_reply(
    sd_bus_message *message,
    void *userdata,
    sd_bus_error *ret_error
)
{
    Application *application = userdata;
    const sd_bus_error *error;

    (void)ret_error;
    if (!sd_bus_message_is_method_error(message, NULL))
        return 0;

    error = sd_bus_message_get_error(message);
    sabg_write_tracker_clear(&application->keyboard_write_tracker);
    fprintf(
        stderr,
        "Unable to set keyboard brightness: %s\n",
        error != NULL && error->message != NULL ? error->message : "unknown D-Bus error"
    );
    return 0;
}

static int write_keyboard_brightness(Application *application, int percentage)
{
    sd_bus_message *message = NULL;
    int result;

    if (application->configuration.dry_run) {
        if (application->configuration.verbose)
            fprintf(stderr, "dry-run keyboard brightness: %d%%\n", percentage);
        return 0;
    }

    if (application->keyboard_backend == KEYBOARD_BACKEND_GNOME) {
        result = sd_bus_message_new_method_call(
            application->session_bus,
            &message,
            GNOME_POWER_DESTINATION,
            GNOME_POWER_PATH,
            "org.freedesktop.DBus.Properties",
            "Set"
        );
        if (result >= 0)
            result = sd_bus_message_append(message, "ss", GNOME_KEYBOARD_INTERFACE, "Brightness");
        if (result >= 0)
            result = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "i");
        if (result >= 0)
            result = sd_bus_message_append(message, "i", (int32_t)percentage);
        if (result >= 0)
            result = sd_bus_message_close_container(message);
    } else if (application->keyboard_backend == KEYBOARD_BACKEND_UPOWER) {
        result = sd_bus_message_new_method_call(
            application->system_bus,
            &message,
            UPOWER_DESTINATION,
            application->keyboard_object_path,
            UPOWER_KEYBOARD_INTERFACE,
            "SetBrightness"
        );
        if (result >= 0) {
            result = sd_bus_message_append(
                message,
                "i",
                (int32_t)percentage_to_raw(percentage, application->keyboard_maximum_raw)
            );
        }
    } else {
        return -ENODEV;
    }

    if (result >= 0) {
        sd_bus *bus = application->keyboard_backend == KEYBOARD_BACKEND_GNOME
            ? application->session_bus
            : application->system_bus;
        result = sd_bus_call_async(
            bus,
            NULL,
            message,
            on_keyboard_write_reply,
            application,
            0
        );
        if (result >= 0) {
            sabg_write_tracker_record(
                &application->keyboard_write_tracker,
                percentage,
                monotonic_usec()
            );
        }
    }
    message = sd_bus_message_unref(message);
    if (result < 0)
        fprintf(stderr, "Unable to queue keyboard brightness: %s\n", strerror(-result));
    return result;
}

static int schedule_keyboard_animation(Application *application, uint64_t wakeup_usec);

static int on_keyboard_animation_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    int percentage;
    int result;

    (void)source;
    (void)usec;
    percentage = sabg_smoother_advance(&application->keyboard_smoother, monotonic_usec());
    result = write_keyboard_brightness(application, percentage);
    if (result < 0)
        return result;

    if (sabg_smoother_active(&application->keyboard_smoother)) {
        return schedule_keyboard_animation(
            application,
            sabg_smoother_next_wakeup_usec(&application->keyboard_smoother)
        );
    }

    if (application->configuration.verbose)
        fprintf(stderr, "keyboard transition complete at %d%%\n", percentage);
    return 0;
}

static int schedule_keyboard_animation(Application *application, uint64_t wakeup_usec)
{
    int result;

    if (application->keyboard_animation_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->keyboard_animation_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_keyboard_animation_timer,
            application
        );
    }

    result = sd_event_source_set_time(application->keyboard_animation_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(
        application->keyboard_animation_timer,
        SD_EVENT_ONESHOT
    );
}

static int update_keyboard_target(Application *application, double lux, uint64_t now_usec)
{
    int target;
    int result;

    if (!application->keyboard_model_ready)
        return 0;
    if (!sabg_keyboard_model_observe(&application->keyboard_model, lux, now_usec, &target))
        return 0;

    if (application->configuration.verbose)
        fprintf(stderr, "ambient %.2f lux -> keyboard target %d%%\n", lux, target);
    if (!sabg_smoother_set_target(&application->keyboard_smoother, target, now_usec))
        return 0;
    if (!sabg_smoother_active(&application->keyboard_smoother)) {
        if (application->keyboard_animation_timer != NULL)
            return sd_event_source_set_enabled(application->keyboard_animation_timer, SD_EVENT_OFF);
        return 0;
    }

    if (application->configuration.verbose) {
        fprintf(
            stderr,
            "keyboard transition %d%% -> %d%% in %llu ms across %u frames\n",
            application->keyboard_smoother.start,
            application->keyboard_smoother.target,
            (unsigned long long)(application->keyboard_smoother.duration_usec / UINT64_C(1000)),
            application->keyboard_smoother.frame_count
        );
    }
    result = schedule_keyboard_animation(
        application,
        sabg_smoother_next_wakeup_usec(&application->keyboard_smoother)
    );
    return result;
}

static int on_keyboard_brightness(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    const char *source = "unknown";
    int32_t raw = 0;
    int percentage;
    int result;

    (void)error;
    result = sd_bus_message_read(message, "is", &raw, &source);
    if (result < 0)
        return result;
    percentage = application->keyboard_backend == KEYBOARD_BACKEND_UPOWER
        ? raw_to_percentage((int)raw, application->keyboard_maximum_raw)
        : (int)raw;

    if (sabg_write_tracker_consume(
        &application->keyboard_write_tracker,
        percentage,
        monotonic_usec()
    )) {
        return 0;
    }
    if (!application->keyboard_model_ready)
        return 0;

    sabg_write_tracker_clear(&application->keyboard_write_tracker);
    sabg_smoother_reset(&application->keyboard_smoother, percentage);
    if (application->keyboard_animation_timer != NULL)
        sd_event_source_set_enabled(application->keyboard_animation_timer, SD_EVENT_OFF);
    sabg_keyboard_model_manual_change(
        &application->keyboard_model,
        application->current_lux,
        percentage,
        monotonic_usec()
    );
    if (application->configuration.verbose) {
        if (percentage == 0) {
            fprintf(stderr, "manual keyboard off (%s); keyboard automation suspended\n", source);
        } else {
            fprintf(
                stderr,
                "manual keyboard brightness %d%% (%s); keyboard baseline recalibrated\n",
                percentage,
                source
            );
        }
    }
    return 0;
}

static int on_sensor_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    double lux = 0.0;
    uint64_t now_usec;
    int target;
    int result;

    (void)error;
    result = read_changed_double(message, SENSOR_INTERFACE, "LightLevel", &lux);
    if (result <= 0)
        return result;
    if (!isfinite(lux) || lux < 0.0)
        return 0;

    application->current_lux = lux;
    if (!application->model_ready)
        return 0;

    now_usec = monotonic_usec();
    target = sabg_ambient_model_observe(&application->ambient, lux, now_usec);
    if (application->configuration.verbose)
        fprintf(stderr, "ambient %.2f lux -> target %d%%\n", lux, target);

    if (sabg_smoother_set_target(&application->smoother, target, now_usec)) {
        if (sabg_smoother_active(&application->smoother)) {
            if (application->configuration.verbose) {
                fprintf(
                    stderr,
                    "transition %d%% -> %d%% in %llu ms across %u frames\n",
                    application->smoother.start,
                    application->smoother.target,
                    (unsigned long long)(application->smoother.duration_usec / UINT64_C(1000)),
                    application->smoother.frame_count
                );
            }
            result = schedule_animation(
                application,
                sabg_smoother_next_wakeup_usec(&application->smoother)
            );
            if (result < 0)
                return result;
        } else if (application->animation_timer != NULL) {
            result = sd_event_source_set_enabled(application->animation_timer, SD_EVENT_OFF);
            if (result < 0)
                return result;
        }
    }
    return update_keyboard_target(application, lux, now_usec);
}

static int on_brightness_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    int percentage = 0;
    int result;

    (void)error;
    result = read_changed_int(message, GNOME_SCREEN_INTERFACE, "Brightness", &percentage);
    if (result <= 0)
        return result;

    if (sabg_write_tracker_consume(
        &application->write_tracker,
        percentage,
        monotonic_usec()
    )) {
        return 0;
    }

    if (!application->model_ready)
        return 0;

    sabg_write_tracker_clear(&application->write_tracker);
    sabg_smoother_reset(&application->smoother, percentage);
    if (application->animation_timer != NULL)
        sd_event_source_set_enabled(application->animation_timer, SD_EVENT_OFF);
    sabg_ambient_model_recalibrate(
        &application->ambient,
        application->current_lux,
        percentage,
        monotonic_usec()
    );
    if (application->configuration.verbose)
        fprintf(stderr, "manual brightness %d%%; ambient baseline recalibrated\n", percentage);
    return 0;
}

static int get_initial_state(Application *application, double *lux, int *brightness)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int has_ambient = 0;
    int32_t brightness_value = 0;
    int result;

    result = sd_bus_get_property_trivial(
        application->system_bus,
        SENSOR_DESTINATION,
        SENSOR_PATH,
        SENSOR_INTERFACE,
        "HasAmbientLight",
        &error,
        'b',
        &has_ambient
    );
    if (result < 0)
        goto out;
    if (!has_ambient) {
        result = -ENODEV;
        goto out;
    }

    result = sd_bus_get_property_trivial(
        application->system_bus,
        SENSOR_DESTINATION,
        SENSOR_PATH,
        SENSOR_INTERFACE,
        "LightLevel",
        &error,
        'd',
        lux
    );
    if (result < 0)
        goto out;

    result = sd_bus_get_property_trivial(
        application->session_bus,
        GNOME_POWER_DESTINATION,
        GNOME_POWER_PATH,
        GNOME_SCREEN_INTERFACE,
        "Brightness",
        &error,
        'i',
        &brightness_value
    );
    if (result >= 0)
        *brightness = (int)brightness_value;

out:
    if (result < 0)
        fprintf(stderr, "Unable to read initial brightness state: %s\n",
            error.message != NULL ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    return result;
}

static int claim_light_sensor(Application *application)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result;

    result = sd_bus_call_method(
        application->system_bus,
        SENSOR_DESTINATION,
        SENSOR_PATH,
        SENSOR_INTERFACE,
        "ClaimLight",
        &error,
        NULL,
        ""
    );
    if (result >= 0)
        application->light_claimed = true;
    else
        fprintf(stderr, "Unable to claim ambient sensor: %s\n",
            error.message != NULL ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    return result;
}

static void release_light_sensor(Application *application)
{
    if (!application->light_claimed || application->system_bus == NULL)
        return;

    (void)sd_bus_call_method(
        application->system_bus,
        SENSOR_DESTINATION,
        SENSOR_PATH,
        SENSOR_INTERFACE,
        "ReleaseLight",
        NULL,
        NULL,
        ""
    );
    application->light_claimed = false;
}

static int on_exit_signal(sd_event_source *source, const struct signalfd_siginfo *signal_info, void *userdata)
{
    sd_event *event = userdata;

    (void)source;
    (void)signal_info;
    return sd_event_exit(event, 0);
}

static int application_start(Application *application)
{
    double lux = 0.0;
    int brightness = 0;
    int keyboard_brightness = 0;
    uint64_t now_usec;
    int result;

    result = sd_event_default(&application->event);
    if (result < 0)
        return result;
    result = sd_bus_open_system(&application->system_bus);
    if (result < 0)
        return result;
    result = sd_bus_open_user(&application->session_bus);
    if (result < 0)
        return result;
    result = sd_bus_attach_event(application->system_bus, application->event, 0);
    if (result < 0)
        return result;
    result = sd_bus_attach_event(application->session_bus, application->event, 0);
    if (result < 0)
        return result;

    if (application->configuration.apple_keepalive) {
        result = sabg_apple_als_keepalive_start(
            &application->keepalive,
            application->event,
            application->configuration.apple_als_path,
            application->configuration.apple_refresh_ms
        );
        if (result < 0) {
            fprintf(stderr, "Unable to start Apple ALS keepalive: %s\n", strerror(-result));
            return result;
        }
        fprintf(stderr, "Apple ALS keepalive: %s every %u ms\n",
            application->keepalive.path,
            application->configuration.apple_refresh_ms);
    }

    result = claim_light_sensor(application);
    if (result < 0)
        return result;
    result = get_initial_state(application, &lux, &brightness);
    if (result < 0)
        return result;
    result = discover_keyboard_backlight(application, &keyboard_brightness);
    if (result < 0 && result != -EOPNOTSUPP) {
        fprintf(stderr, "Keyboard backlight unavailable; display control remains active\n");
        application->keyboard_backend = KEYBOARD_BACKEND_NONE;
    }

    printf("SensorProxy: %.2f lux\n", lux);
    printf("GNOME brightness: %d%%\n", brightness);
    if (application->keyboard_backend != KEYBOARD_BACKEND_NONE) {
        printf(
            "Keyboard brightness: %d%% via %s\n",
            keyboard_brightness,
            keyboard_backend_name(application)
        );
    } else {
        printf("Keyboard brightness: unavailable\n");
    }
    if (application->configuration.check_only)
        return 1;

    application->current_lux = lux;
    now_usec = monotonic_usec();
    sabg_smoother_init(
        &application->smoother,
        brightness,
        application->configuration.brighten_step_ms,
        application->configuration.dim_step_ms,
        application->configuration.maximum_transition_ms
    );
    sabg_ambient_model_init(
        &application->ambient,
        lux,
        brightness,
        application->configuration.ambient_time_constant_seconds,
        application->configuration.minimum_percentage,
        application->configuration.maximum_percentage,
        now_usec
    );
    application->model_ready = true;

    if (application->keyboard_backend != KEYBOARD_BACKEND_NONE) {
        sabg_smoother_init(
            &application->keyboard_smoother,
            keyboard_brightness,
            application->configuration.brighten_step_ms,
            application->configuration.dim_step_ms,
            application->configuration.maximum_transition_ms
        );
        sabg_keyboard_model_init(
            &application->keyboard_model,
            lux,
            keyboard_brightness,
            application->configuration.ambient_time_constant_seconds,
            now_usec
        );
        application->keyboard_model_ready = true;
    }

    result = sd_bus_match_signal(
        application->system_bus,
        &application->sensor_properties_slot,
        SENSOR_DESTINATION,
        SENSOR_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        on_sensor_properties,
        application
    );
    if (result < 0)
        return result;
    result = sd_bus_match_signal(
        application->session_bus,
        &application->brightness_properties_slot,
        GNOME_POWER_DESTINATION,
        GNOME_POWER_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        on_brightness_properties,
        application
    );
    if (result < 0)
        return result;

    if (application->keyboard_backend == KEYBOARD_BACKEND_GNOME) {
        result = sd_bus_match_signal(
            application->session_bus,
            &application->keyboard_brightness_slot,
            GNOME_POWER_DESTINATION,
            GNOME_POWER_PATH,
            GNOME_KEYBOARD_INTERFACE,
            "BrightnessChanged",
            on_keyboard_brightness,
            application
        );
    } else if (application->keyboard_backend == KEYBOARD_BACKEND_UPOWER) {
        result = sd_bus_match_signal(
            application->system_bus,
            &application->keyboard_brightness_slot,
            UPOWER_DESTINATION,
            application->keyboard_object_path,
            UPOWER_KEYBOARD_INTERFACE,
            "BrightnessChangedWithSource",
            on_keyboard_brightness,
            application
        );
    }
    if (result < 0)
        return result;

    return 0;
}

static void application_destroy(Application *application)
{
    release_light_sensor(application);
    sabg_apple_als_keepalive_destroy(&application->keepalive);
    application->animation_timer = sd_event_source_unref(application->animation_timer);
    application->keyboard_animation_timer = sd_event_source_unref(application->keyboard_animation_timer);
    application->sensor_properties_slot = sd_bus_slot_unref(application->sensor_properties_slot);
    application->brightness_properties_slot = sd_bus_slot_unref(application->brightness_properties_slot);
    application->keyboard_brightness_slot = sd_bus_slot_unref(application->keyboard_brightness_slot);
    if (application->system_bus != NULL)
        sd_bus_detach_event(application->system_bus);
    if (application->session_bus != NULL)
        sd_bus_detach_event(application->session_bus);
    application->system_bus = sd_bus_unref(application->system_bus);
    application->session_bus = sd_bus_unref(application->session_bus);
    application->event = sd_event_unref(application->event);
}

int main(int argc, char **argv)
{
    Application application = {0};
    sigset_t signal_mask;
    int result;

    application.keepalive.fd = -1;
    result = parse_arguments(argc, argv, &application.configuration);
    if (result < 0) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &signal_mask, NULL) < 0) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    result = application_start(&application);
    if (result == 1) {
        application_destroy(&application);
        return EXIT_SUCCESS;
    }
    if (result < 0) {
        application_destroy(&application);
        return EXIT_FAILURE;
    }

    result = sd_event_add_signal(application.event, NULL, SIGINT, on_exit_signal, application.event);
    if (result >= 0)
        result = sd_event_add_signal(application.event, NULL, SIGTERM, on_exit_signal, application.event);
    if (result >= 0) {
        fprintf(stderr, "smooth ambient brightness active\n");
        result = sd_event_loop(application.event);
    }

    if (result < 0)
        fprintf(stderr, "Event loop failed: %s\n", strerror(-result));
    application_destroy(&application);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
