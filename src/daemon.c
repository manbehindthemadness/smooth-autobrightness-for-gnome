// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_als_keepalive.h"
#include "sabg/ambient_model.h"
#include "sabg/display_response.h"
#include "sabg/frame_scheduler.h"
#include "sabg/keyboard_model.h"
#include "sabg/lux_average.h"
#include "sabg/output_quantizer.h"
#include "sabg/smoother.h"
#include "sabg/suspend_guard.h"
#include "sabg/target_hysteresis.h"
#include "sabg/trajectory.h"
#include "sabg/user_brightness.h"
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

#define SENSOR_DESTINATION "net.hadess.SensorProxy"
#define SENSOR_PATH "/net/hadess/SensorProxy"
#define SENSOR_INTERFACE "net.hadess.SensorProxy"

#define DBUS_DESTINATION "org.freedesktop.DBus"
#define DBUS_PATH "/org/freedesktop/DBus"
#define DBUS_INTERFACE "org.freedesktop.DBus"

#define GNOME_POWER_DESTINATION "org.gnome.SettingsDaemon.Power"
#define GNOME_POWER_PATH "/org/gnome/SettingsDaemon/Power"
#define GNOME_SCREEN_INTERFACE "org.gnome.SettingsDaemon.Power.Screen"
#define GNOME_KEYBOARD_INTERFACE "org.gnome.SettingsDaemon.Power.Keyboard"

#define MUTTER_DISPLAY_DESTINATION "org.gnome.Mutter.DisplayConfig"
#define MUTTER_DISPLAY_PATH "/org/gnome/Mutter/DisplayConfig"
#define MUTTER_DISPLAY_INTERFACE "org.gnome.Mutter.DisplayConfig"
#define MUTTER_POWER_SAVE_ON 0

#define UPOWER_DESTINATION "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_INTERFACE "org.freedesktop.UPower"
#define UPOWER_KEYBOARD_PATH "/org/freedesktop/UPower/KbdBacklight"
#define UPOWER_KEYBOARD_INTERFACE "org.freedesktop.UPower.KbdBacklight"
#define LOGIND_DESTINATION "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_INTERFACE "org.freedesktop.login1.Manager"
#define RESUME_RESTORE_DELAY_USEC UINT64_C(1000000)
#define RESUME_SENSOR_SETTLE_USEC UINT64_C(2000000)
#define SENSOR_OWNER_SETTLE_USEC UINT64_C(250000)
#define SENSOR_REFRESH_RETRY_USEC UINT64_C(500000)
#define SENSOR_REFRESH_MAX_ATTEMPTS 10U
#define SENSOR_SAMPLE_INTERVAL_USEC UINT64_C(500000)
#define MOTION_MINIMUM_UPDATE_HZ 2U
#define MOTION_MAXIMUM_UPDATE_HZ 60U
#define MOTION_MAXIMUM_STEP_PER_FRAME 0.2
/* Dimming needs a shorter envelope because sparse low-end panel steps expose its tail. */
#define TRAJECTORY_DIMMING_TIME_CONSTANT_FACTOR (1.0 / 3.0)
#define TRAJECTORY_DIMMING_FINISH_DISTANCE 4.0
#define LARGE_CHANGE_THRESHOLD 12.0
#define LARGE_CHANGE_TIME_CONSTANT_FACTOR 0.14
#define LARGE_CHANGE_FINISH_DISTANCE 4.0

typedef enum {
    KEYBOARD_BACKEND_NONE,
    KEYBOARD_BACKEND_GNOME,
    KEYBOARD_BACKEND_UPOWER,
} KeyboardBackend;

typedef struct {
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
    unsigned int maximum_transition_ms;
    unsigned int hysteresis_percentage;
    unsigned int apple_refresh_ms;
    double ambient_time_constant_seconds;
    double sensor_average_seconds;
    int minimum_percentage;
    int maximum_percentage;
    bool keyboard_backlight;
    bool legacy_transitions;
    bool apple_keepalive;
    bool check_only;
    bool dry_run;
    bool verbose;
    const char *apple_als_path;
    const char *calibration_profile;
    bool calibration_profile_disabled;
} Configuration;

typedef struct {
    Configuration configuration;
    sd_event *event;
    sd_event_source *animation_timer;
    sd_event_source *keyboard_animation_timer;
    sd_event_source *motion_timer;
    sd_event_source *resume_restore_timer;
    sd_event_source *sensor_refresh_timer;
    sd_event_source *sensor_sample_timer;
    sd_bus *system_bus;
    sd_bus *session_bus;
    sd_bus_slot *sensor_properties_slot;
    sd_bus_slot *sensor_owner_slot;
    sd_bus_slot *brightness_properties_slot;
    sd_bus_slot *keyboard_brightness_slot;
    sd_bus_slot *display_power_properties_slot;
    sd_bus_slot *lid_properties_slot;
    sd_bus_slot *prepare_for_sleep_slot;
    SabgAmbientModel ambient;
    SabgSmoother smoother;
    SabgTargetHysteresis target_hysteresis;
    SabgWriteTracker write_tracker;
    SabgKeyboardModel keyboard_model;
    SabgSmoother keyboard_smoother;
    SabgTargetHysteresis keyboard_target_hysteresis;
    SabgWriteTracker keyboard_write_tracker;
    SabgTrajectory display_trajectory;
    SabgDisplayResponse display_response;
    SabgTrajectory keyboard_trajectory;
    SabgOutputQuantizer display_quantizer;
    SabgOutputQuantizer keyboard_quantizer;
    SabgAppleAlsKeepalive keepalive;
    SabgSuspendGuard suspend_guard;
    SabgLuxAverage lux_average;
    double raw_lux;
    double current_lux;
    uint64_t sensor_average_until_usec;
    bool light_claimed;
    unsigned int sensor_refresh_attempts;
    bool model_ready;
    KeyboardBackend keyboard_backend;
    char keyboard_object_path[PATH_MAX];
    int keyboard_maximum_raw;
    bool keyboard_model_ready;
    bool display_powered_down;
    int last_user_display_brightness;
    int last_user_keyboard_brightness;
    char user_brightness_path[PATH_MAX];
} Application;

static int schedule_sensor_refresh(Application *application, uint64_t wakeup_usec);
static int schedule_sensor_sample(Application *application, uint64_t wakeup_usec);

static uint64_t monotonic_usec(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000)
        + (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static uint64_t sensor_average_window_usec(const Application *application)
{
    return (uint64_t)(
        application->configuration.sensor_average_seconds * 1000000.0
    );
}

static const char *default_calibration_profile(char path[PATH_MAX])
{
    const char *root = getenv("XDG_CONFIG_HOME");
    int written;

    if (root != NULL && root[0] != '\0') {
        written = snprintf(
            path,
            PATH_MAX,
            "%s/smooth-autobrightness-for-gnome/display-calibration.json",
            root
        );
    } else {
        root = getenv("HOME");
        if (root == NULL || root[0] == '\0')
            return NULL;
        written = snprintf(
            path,
            PATH_MAX,
            "%s/.config/smooth-autobrightness-for-gnome/display-calibration.json",
            root
        );
    }
    return written >= 0 && written < PATH_MAX ? path : NULL;
}

static bool configure_user_brightness_path(Application *application)
{
    const char *state_directory = getenv("STATE_DIRECTORY");
    int written;

    if (state_directory == NULL || state_directory[0] == '\0')
        return false;
    written = snprintf(
        application->user_brightness_path,
        sizeof(application->user_brightness_path),
        "%s/brightness-state",
        state_directory
    );
    return written >= 0
        && (size_t)written < sizeof(application->user_brightness_path);
}

static void save_user_brightness(Application *application)
{
    SabgUserBrightness brightness = {
        .display = application->last_user_display_brightness,
        .keyboard = application->last_user_keyboard_brightness,
    };
    int result;

    if (application->user_brightness_path[0] == '\0')
        return;
    result = sabg_user_brightness_save(application->user_brightness_path, &brightness);
    if (result < 0) {
        fprintf(
            stderr,
            "Unable to save user brightness state: %s\n",
            strerror(-result)
        );
    }
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
        "  --hysteresis N             target change threshold in %% (default: 2)\n"
        "  --ambient-time-constant S  target filter time constant (default: 1.6)\n"
        "  --sensor-average-seconds S rolling lux window (default: 10.0)\n"
        "  --min-brightness N         automatic floor (default: 2)\n"
        "  --max-brightness N         automatic ceiling (default: 100)\n"
        "  --no-keyboard-backlight    leave keyboard illumination unchanged\n"
        "  --legacy-transitions       use the pre-0.4 restarted-fade controller\n"
        "  --apple-als-keepalive      enable optional Apple IIO refreshes\n"
        "  --apple-als-path PATH      refresh a specific IIO illuminance file\n"
        "  --apple-refresh-ms N       refresh interval (default: 500)\n"
        "  --calibration-profile PATH use a display response profile\n"
        "  --no-calibration-profile  ignore the default display response profile\n"
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
        OPTION_HYSTERESIS,
        OPTION_AMBIENT_TIME_CONSTANT,
        OPTION_SENSOR_AVERAGE_SECONDS,
        OPTION_MIN_BRIGHTNESS,
        OPTION_MAX_BRIGHTNESS,
        OPTION_NO_KEYBOARD_BACKLIGHT,
        OPTION_LEGACY_TRANSITIONS,
        OPTION_APPLE_KEEPALIVE,
        OPTION_APPLE_PATH,
        OPTION_APPLE_REFRESH,
        OPTION_CALIBRATION_PROFILE,
        OPTION_NO_CALIBRATION_PROFILE,
        OPTION_CHECK,
        OPTION_DRY_RUN,
        OPTION_VERBOSE,
        OPTION_VERSION,
    };
    static const struct option options[] = {
        {"brighten-step-ms", required_argument, NULL, OPTION_BRIGHTEN_STEP},
        {"dim-step-ms", required_argument, NULL, OPTION_DIM_STEP},
        {"max-transition-ms", required_argument, NULL, OPTION_MAX_TRANSITION},
        {"hysteresis", required_argument, NULL, OPTION_HYSTERESIS},
        {"ambient-time-constant", required_argument, NULL, OPTION_AMBIENT_TIME_CONSTANT},
        {"sensor-average-seconds", required_argument, NULL, OPTION_SENSOR_AVERAGE_SECONDS},
        {"min-brightness", required_argument, NULL, OPTION_MIN_BRIGHTNESS},
        {"max-brightness", required_argument, NULL, OPTION_MAX_BRIGHTNESS},
        {"no-keyboard-backlight", no_argument, NULL, OPTION_NO_KEYBOARD_BACKLIGHT},
        {"legacy-transitions", no_argument, NULL, OPTION_LEGACY_TRANSITIONS},
        {"apple-als-keepalive", no_argument, NULL, OPTION_APPLE_KEEPALIVE},
        {"apple-als-path", required_argument, NULL, OPTION_APPLE_PATH},
        {"apple-refresh-ms", required_argument, NULL, OPTION_APPLE_REFRESH},
        {"calibration-profile", required_argument, NULL, OPTION_CALIBRATION_PROFILE},
        {"no-calibration-profile", no_argument, NULL, OPTION_NO_CALIBRATION_PROFILE},
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
        .dim_step_ms = 40,
        .maximum_transition_ms = 250,
        .hysteresis_percentage = 2,
        .apple_refresh_ms = 500,
        .ambient_time_constant_seconds = 1.6,
        .sensor_average_seconds = 10.0,
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
        case OPTION_HYSTERESIS:
            if (parse_int(optarg, 0, 100, &parsed) < 0)
                return -EINVAL;
            configuration->hysteresis_percentage = (unsigned int)parsed;
            break;
        case OPTION_AMBIENT_TIME_CONSTANT:
            if (parse_double(optarg, 0.01, 300.0, &configuration->ambient_time_constant_seconds) < 0)
                return -EINVAL;
            break;
        case OPTION_SENSOR_AVERAGE_SECONDS:
            if (parse_double(optarg, 0.25, 30.0, &configuration->sensor_average_seconds) < 0)
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
        case OPTION_LEGACY_TRANSITIONS:
            configuration->legacy_transitions = true;
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
        case OPTION_CALIBRATION_PROFILE:
            configuration->calibration_profile = optarg;
            configuration->calibration_profile_disabled = false;
            break;
        case OPTION_NO_CALIBRATION_PROFILE:
            configuration->calibration_profile_disabled = true;
            configuration->calibration_profile = NULL;
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
            printf("smooth-autobrightness-for-gnome %s\n", SABG_VERSION);
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

static int read_changed_bool(
    sd_bus_message *message,
    const char *expected_interface,
    const char *property,
    bool *value
)
{
    const char *interface = NULL;
    int property_value = 0;
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
            result = sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, "b");
            if (result < 0)
                return result;
            result = sd_bus_message_read(message, "b", &property_value);
            if (result < 0)
                return result;
            result = sd_bus_message_exit_container(message);
            if (result < 0)
                return result;
            *value = property_value != 0;
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
        size_t path_length = strlen(path);

        if (path_length >= sizeof(application->keyboard_object_path))
            result = -ENAMETOOLONG;
        else
            memcpy(application->keyboard_object_path, path, path_length + 1U);
    } else if (result >= 0) {
        result = -ENODEV;
    }
    reply = sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    if (result < 0) {
        memcpy(
            application->keyboard_object_path,
            UPOWER_KEYBOARD_PATH,
            sizeof(UPOWER_KEYBOARD_PATH)
        );
    }
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

    if (!application->keyboard_model_ready || application->display_powered_down)
        return 0;
    if (!sabg_keyboard_model_observe(&application->keyboard_model, lux, now_usec, &target))
        return 0;

    if (application->configuration.verbose)
        fprintf(stderr, "ambient %.2f lux -> keyboard target %d%%\n", lux, target);
    if (!sabg_target_hysteresis_accept(
        &application->keyboard_target_hysteresis,
        target
    )) {
        return 0;
    }
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
    if (application->display_powered_down) {
        if (percentage != 0)
            return write_keyboard_brightness(application, 0);
        return 0;
    }
    if (!application->keyboard_model_ready)
        return 0;

    application->last_user_keyboard_brightness = percentage;
    save_user_brightness(application);
    sabg_write_tracker_clear(&application->keyboard_write_tracker);
    sabg_smoother_reset(&application->keyboard_smoother, percentage);
    sabg_target_hysteresis_reset(
        &application->keyboard_target_hysteresis,
        percentage
    );
    if (!application->configuration.legacy_transitions) {
        sabg_trajectory_reset(
            &application->keyboard_trajectory,
            percentage,
            monotonic_usec()
        );
        sabg_output_quantizer_reset(&application->keyboard_quantizer, percentage);
    }
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

static bool motion_active(const Application *application)
{
    if (sabg_suspend_guard_blocked(&application->suspend_guard))
        return false;
    return sabg_ambient_model_active(&application->ambient)
        || sabg_trajectory_active(&application->display_trajectory)
        || (application->keyboard_model_ready
            && !application->display_powered_down
            && (sabg_keyboard_model_active(&application->keyboard_model)
                || sabg_trajectory_active(&application->keyboard_trajectory)));
}

static int schedule_motion_update(Application *application, uint64_t now_usec);

static unsigned int trajectory_update_rate(
    const SabgTrajectory *trajectory,
    double model_velocity,
    uint64_t now_usec
)
{
    uint64_t remaining_usec = trajectory->deadline_usec > now_usec
        ? trajectory->deadline_usec - now_usec
        : 0;

    return sabg_frame_scheduler_rate(
        fabs(model_velocity) > fabs(trajectory->velocity)
            ? model_velocity
            : trajectory->velocity,
        trajectory->target - trajectory->position,
        remaining_usec,
        MOTION_MINIMUM_UPDATE_HZ,
        MOTION_MAXIMUM_UPDATE_HZ,
        MOTION_MAXIMUM_STEP_PER_FRAME
    );
}

static unsigned int motion_update_rate(
    const Application *application,
    uint64_t now_usec
)
{
    double display_position = sabg_display_response_to_brightness(
        &application->display_response,
        application->display_trajectory.position
    );
    unsigned int rate = trajectory_update_rate(
        &application->display_trajectory,
        sabg_ambient_model_velocity(&application->ambient)
            * sabg_display_response_velocity_scale(
                &application->display_response,
                display_position
            ),
        now_usec
    );

    if (application->keyboard_model_ready && !application->display_powered_down) {
        unsigned int keyboard_rate = trajectory_update_rate(
            &application->keyboard_trajectory,
            sabg_keyboard_model_velocity(&application->keyboard_model),
            now_usec
        );
        if (keyboard_rate > rate)
            rate = keyboard_rate;
    }
    return rate;
}

static int update_motion(
    Application *application,
    double lux,
    uint64_t now_usec,
    bool log_target
)
{
    double display_target;
    double display_target_coordinate;
    double keyboard_target = 0.0;
    double position;
    int output;
    int result;

    display_target = sabg_ambient_model_advance(&application->ambient, lux, now_usec);
    display_target_coordinate = sabg_display_response_to_coordinate(
        &application->display_response,
        display_target
    );
    sabg_trajectory_set_target(
        &application->display_trajectory,
        display_target_coordinate,
        now_usec
    );
    position = sabg_display_response_to_brightness(
        &application->display_response,
        sabg_trajectory_advance(&application->display_trajectory, now_usec)
    );
    if (log_target && application->configuration.verbose) {
        fprintf(
            stderr,
            "ambient %.2f lux -> envelope %.2f%%, display %.2f%% at %.2f%%/s\n",
            lux,
            display_target,
            position,
            application->display_trajectory.velocity
                / sabg_display_response_velocity_scale(
                    &application->display_response,
                    position
                )
        );
    }
    if (sabg_output_quantizer_update(&application->display_quantizer, position, &output)) {
        result = write_brightness(application, output);
        if (result < 0)
            return result;
    }

    if (application->keyboard_model_ready
        && !application->display_powered_down
        && sabg_keyboard_model_advance(
            &application->keyboard_model,
            lux,
            now_usec,
            &keyboard_target
        )) {
        sabg_trajectory_set_target(
            &application->keyboard_trajectory,
            keyboard_target,
            now_usec
        );
        position = sabg_trajectory_advance(&application->keyboard_trajectory, now_usec);
        if (log_target && application->configuration.verbose) {
            fprintf(
                stderr,
                "ambient %.2f lux -> keyboard envelope %.2f%%, position %.2f%% at %.2f%%/s\n",
                lux,
                keyboard_target,
                position,
                application->keyboard_trajectory.velocity
            );
        }
        if (sabg_output_quantizer_update(
            &application->keyboard_quantizer,
            position,
            &output
        )) {
            result = write_keyboard_brightness(application, output);
            if (result < 0)
                return result;
        }
    }

    if (motion_active(application))
        return schedule_motion_update(application, now_usec);
    return 0;
}

static int on_motion_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    uint64_t now_usec = monotonic_usec();

    (void)source;
    (void)usec;
    if (sabg_suspend_guard_blocked(&application->suspend_guard))
        return 0;
    return update_motion(
        application,
        application->current_lux,
        now_usec,
        false
    );
}

static int schedule_motion_update(Application *application, uint64_t now_usec)
{
    unsigned int update_hz = motion_update_rate(application, now_usec);
    uint64_t wakeup_usec = now_usec + UINT64_C(1000000) / update_hz;
    int result;

    if (application->motion_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->motion_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_motion_timer,
            application
        );
    }
    result = sd_event_source_set_time(application->motion_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(application->motion_timer, SD_EVENT_ONESHOT);
}

static int stop_ambient_motion(Application *application, int brightness, uint64_t now_usec)
{
    int result;

    if (application->animation_timer != NULL) {
        result = sd_event_source_set_enabled(application->animation_timer, SD_EVENT_OFF);
        if (result < 0)
            return result;
    }
    if (application->motion_timer != NULL) {
        result = sd_event_source_set_enabled(application->motion_timer, SD_EVENT_OFF);
        if (result < 0)
            return result;
    }
    sabg_smoother_reset(&application->smoother, brightness);
    sabg_target_hysteresis_reset(&application->target_hysteresis, brightness);
    sabg_trajectory_reset(
        &application->display_trajectory,
        sabg_display_response_to_coordinate(
            &application->display_response,
            (double)brightness
        ),
        now_usec
    );
    sabg_output_quantizer_reset(&application->display_quantizer, brightness);
    return 0;
}

static void reset_keyboard_motion(Application *application, int brightness, uint64_t now_usec)
{
    if (!application->keyboard_model_ready)
        return;
    sabg_smoother_reset(&application->keyboard_smoother, brightness);
    sabg_target_hysteresis_reset(&application->keyboard_target_hysteresis, brightness);
    sabg_trajectory_reset(&application->keyboard_trajectory, brightness, now_usec);
    sabg_output_quantizer_reset(&application->keyboard_quantizer, brightness);
    if (application->keyboard_animation_timer != NULL)
        (void)sd_event_source_set_enabled(application->keyboard_animation_timer, SD_EVENT_OFF);
}

static int restore_user_brightness(Application *application, uint64_t now_usec)
{
    int result;

    result = stop_ambient_motion(
        application,
        application->last_user_display_brightness,
        now_usec
    );
    if (result < 0)
        return result;
    result = write_brightness(application, application->last_user_display_brightness);
    if (result < 0)
        return result;

    if (application->keyboard_model_ready) {
        reset_keyboard_motion(
            application,
            application->last_user_keyboard_brightness,
            now_usec
        );
        result = write_keyboard_brightness(
            application,
            application->last_user_keyboard_brightness
        );
        if (result < 0)
            return result;
    }
    return 0;
}

static int on_resume_restore_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;

    (void)source;
    (void)usec;
    if (application->suspend_guard.lid_closed
        || application->suspend_guard.preparing_sleep) {
        return 0;
    }
    return restore_user_brightness(application, monotonic_usec());
}

static int schedule_resume_restore(Application *application, uint64_t now_usec)
{
    uint64_t wakeup_usec = now_usec + RESUME_RESTORE_DELAY_USEC;
    int result;

    if (application->resume_restore_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->resume_restore_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_resume_restore_timer,
            application
        );
    }
    result = sd_event_source_set_time(application->resume_restore_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(application->resume_restore_timer, SD_EVENT_ONESHOT);
}

static int handle_suspend_transition(
    Application *application,
    SabgSuspendTransition transition,
    uint64_t now_usec
)
{
    int result;

    if (transition.entered) {
        sabg_lux_average_clear(&application->lux_average);
        application->sensor_average_until_usec = 0U;
        if (application->sensor_sample_timer != NULL) {
            result = sd_event_source_set_enabled(
                application->sensor_sample_timer,
                SD_EVENT_OFF
            );
            if (result < 0)
                return result;
        }
        if (application->configuration.apple_keepalive) {
            result = sabg_apple_als_keepalive_set_enabled(
                &application->keepalive,
                false
            );
            if (result < 0)
                return result;
        }
        result = stop_ambient_motion(
            application,
            application->suspend_guard.protected_brightness,
            now_usec
        );
        if (result < 0)
            return result;
        fprintf(
            stderr,
            "ambient control paused; preserving display brightness at %d%%\n",
            application->suspend_guard.protected_brightness
        );
    }
    if (transition.resumed) {
        if (application->configuration.apple_keepalive) {
            result = sabg_apple_als_keepalive_set_enabled(
                &application->keepalive,
                true
            );
            if (result < 0)
                return result;
        }
        result = restore_user_brightness(application, now_usec);
        if (result < 0)
            return result;
        result = schedule_resume_restore(application, now_usec);
        if (result < 0)
            return result;
        result = schedule_sensor_refresh(
            application,
            application->suspend_guard.resume_after_usec
        );
        if (result < 0)
            return result;
        fprintf(
            stderr,
            "restored user brightness: display %d%%, keyboard %d%%; "
            "waiting 2 seconds for ambient sensor\n",
            application->last_user_display_brightness,
            application->last_user_keyboard_brightness
        );
    }
    return 0;
}

static int handle_sensor_sample(Application *application, double lux)
{
    double averaged_lux;
    uint64_t now_usec;
    bool first_after_resume = false;
    int target;
    int result;

    if (!isfinite(lux) || lux < 0.0)
        return 0;

    if (!application->model_ready)
        return 0;

    application->raw_lux = lux;
    now_usec = monotonic_usec();
    if (!sabg_suspend_guard_accept_sample(
        &application->suspend_guard,
        now_usec,
        &first_after_resume
    )) {
        return 0;
    }
    if (first_after_resume) {
        int brightness = application->suspend_guard.protected_brightness;

        sabg_lux_average_reset(&application->lux_average, lux, now_usec);
        application->sensor_average_until_usec = now_usec;
        averaged_lux = lux;
        sabg_ambient_model_resume(
            &application->ambient,
            averaged_lux,
            brightness,
            now_usec
        );
        result = stop_ambient_motion(application, brightness, now_usec);
        if (result < 0)
            return result;
        if (application->configuration.verbose)
            fprintf(
                stderr,
                "ambient sensor settled at %.2f lux; automation resumed\n",
                averaged_lux
            );
    } else {
        averaged_lux = sabg_lux_average_observe(
            &application->lux_average,
            lux,
            now_usec
        );
    }
    application->current_lux = averaged_lux;
    if (!application->configuration.legacy_transitions)
        return update_motion(application, averaged_lux, now_usec, true);
    target = sabg_ambient_model_observe(
        &application->ambient,
        averaged_lux,
        now_usec
    );
    if (application->configuration.verbose)
        fprintf(
            stderr,
            "ambient %.2f raw / %.2f averaged lux -> target %d%%\n",
            lux,
            averaged_lux,
            target
        );

    if (sabg_target_hysteresis_accept(&application->target_hysteresis, target)
        && sabg_smoother_set_target(&application->smoother, target, now_usec)) {
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
    return update_keyboard_target(application, averaged_lux, now_usec);
}

static int on_sensor_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    double lux = 0.0;
    bool resume_sample_pending;
    uint64_t now_usec;
    int result;

    (void)error;
    result = read_changed_double(message, SENSOR_INTERFACE, "LightLevel", &lux);
    if (result <= 0)
        return result;
    if (lux == application->raw_lux)
        return 0;

    resume_sample_pending = application->suspend_guard.resume_sample_pending;
    now_usec = monotonic_usec();
    application->sensor_average_until_usec = now_usec
        + sensor_average_window_usec(application);
    result = handle_sensor_sample(application, lux);
    if (result < 0 || resume_sample_pending)
        return result;
    return schedule_sensor_sample(
        application,
        now_usec + SENSOR_SAMPLE_INTERVAL_USEC
    );
}

static int on_sensor_sample_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    uint64_t now_usec;
    int result;

    (void)usec;
    result = handle_sensor_sample(application, application->raw_lux);
    if (result < 0)
        return result;
    now_usec = monotonic_usec();
    if (now_usec >= application->sensor_average_until_usec)
        return 0;
    result = sd_event_source_set_time(
        source,
        now_usec + SENSOR_SAMPLE_INTERVAL_USEC
    );
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
}

static int schedule_sensor_sample(Application *application, uint64_t wakeup_usec)
{
    int result;

    if (application->sensor_sample_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->sensor_sample_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_sensor_sample_timer,
            application
        );
    }
    result = sd_event_source_set_time(application->sensor_sample_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(application->sensor_sample_timer, SD_EVENT_ONESHOT);
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

    if (sabg_suspend_guard_blocked(&application->suspend_guard))
        return 0;

    application->last_user_display_brightness = percentage;
    save_user_brightness(application);
    sabg_write_tracker_clear(&application->write_tracker);
    sabg_smoother_reset(&application->smoother, percentage);
    sabg_target_hysteresis_reset(&application->target_hysteresis, percentage);
    if (!application->configuration.legacy_transitions) {
        sabg_trajectory_reset(
            &application->display_trajectory,
            sabg_display_response_to_coordinate(
                &application->display_response,
                (double)percentage
            ),
            monotonic_usec()
        );
        sabg_output_quantizer_reset(&application->display_quantizer, percentage);
    }
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

static int set_display_powered_down(Application *application, bool powered_down)
{
    double target = 0.0;
    uint64_t now_usec;
    int result;

    if (application->display_powered_down == powered_down)
        return 0;
    application->display_powered_down = powered_down;
    if (!application->keyboard_model_ready)
        return 0;

    now_usec = monotonic_usec();
    if (powered_down) {
        if (application->keyboard_animation_timer != NULL) {
            result = sd_event_source_set_enabled(
                application->keyboard_animation_timer,
                SD_EVENT_OFF
            );
            if (result < 0)
                return result;
        }
        if (application->configuration.verbose)
            fprintf(stderr, "display powered down; keyboard backlight off\n");
        return write_keyboard_brightness(application, 0);
    }

    if (application->configuration.verbose)
        fprintf(stderr, "display powered on; resuming keyboard automation\n");
    if (application->configuration.legacy_transitions) {
        sabg_smoother_reset(&application->keyboard_smoother, 0);
        sabg_target_hysteresis_reset(&application->keyboard_target_hysteresis, 0);
        return update_keyboard_target(application, application->current_lux, now_usec);
    }

    sabg_trajectory_reset(&application->keyboard_trajectory, 0.0, now_usec);
    sabg_output_quantizer_reset(&application->keyboard_quantizer, 0);
    if (!sabg_keyboard_model_advance(
        &application->keyboard_model,
        application->current_lux,
        now_usec,
        &target
    )) {
        return 0;
    }
    sabg_trajectory_set_target(&application->keyboard_trajectory, target, now_usec);
    if (motion_active(application))
        return schedule_motion_update(application, now_usec);
    return 0;
}

static int on_display_power_properties(
    sd_bus_message *message,
    void *userdata,
    sd_bus_error *error
)
{
    Application *application = userdata;
    int mode = MUTTER_POWER_SAVE_ON;
    int result;

    (void)error;
    result = read_changed_int(
        message,
        MUTTER_DISPLAY_INTERFACE,
        "PowerSaveMode",
        &mode
    );
    if (result <= 0)
        return result;
    return set_display_powered_down(application, mode > MUTTER_POWER_SAVE_ON);
}

static int on_lid_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    SabgSuspendTransition transition;
    bool closed = false;
    uint64_t now_usec;
    int result;

    (void)error;
    result = read_changed_bool(message, LOGIND_INTERFACE, "LidClosed", &closed);
    if (result <= 0)
        return result;
    now_usec = monotonic_usec();
    transition = sabg_suspend_guard_set_lid(
        &application->suspend_guard,
        closed,
        application->last_user_display_brightness,
        now_usec
    );
    return handle_suspend_transition(application, transition, now_usec);
}

static int on_prepare_for_sleep(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    SabgSuspendTransition transition;
    int preparing = 0;
    uint64_t now_usec;
    int result;

    (void)error;
    result = sd_bus_message_read(message, "b", &preparing);
    if (result < 0)
        return result;
    now_usec = monotonic_usec();
    transition = sabg_suspend_guard_set_sleep(
        &application->suspend_guard,
        preparing != 0,
        application->last_user_display_brightness,
        now_usec
    );
    return handle_suspend_transition(application, transition, now_usec);
}

static int get_lid_closed(Application *application, bool *closed)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int value = 0;
    int result;

    result = sd_bus_get_property_trivial(
        application->system_bus,
        LOGIND_DESTINATION,
        LOGIND_PATH,
        LOGIND_INTERFACE,
        "LidClosed",
        &error,
        'b',
        &value
    );
    if (result >= 0)
        *closed = value != 0;
    else
        fprintf(stderr, "Unable to read lid state: %s\n",
            error.message != NULL ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    return result;
}

static int get_display_powered_down(Application *application, bool *powered_down)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int32_t mode = MUTTER_POWER_SAVE_ON;
    int result;

    result = sd_bus_get_property_trivial(
        application->session_bus,
        MUTTER_DISPLAY_DESTINATION,
        MUTTER_DISPLAY_PATH,
        MUTTER_DISPLAY_INTERFACE,
        "PowerSaveMode",
        &error,
        'i',
        &mode
    );
    if (result >= 0)
        *powered_down = mode > MUTTER_POWER_SAVE_ON;
    else
        fprintf(stderr, "Unable to read display power state: %s\n",
            error.message != NULL ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    return result;
}

static int get_light_level(Application *application, double *lux)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int has_ambient = 0;
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

out:
    sd_bus_error_free(&error);
    return result;
}

static int get_initial_state(Application *application, double *lux, int *brightness)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int32_t brightness_value = 0;
    int result;

    result = get_light_level(application, lux);
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

static int on_sensor_refresh_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    sd_event *event = sd_event_source_get_event(source);
    double lux = 0.0;
    bool changed;
    bool resume_sample_pending;
    uint64_t now_usec = usec;
    int result;

    if (!application->light_claimed) {
        result = claim_light_sensor(application);
        if (result < 0)
            goto retry;
    }
    result = get_light_level(application, &lux);
    if (result < 0)
        goto retry;

    application->sensor_refresh_attempts = 0;
    changed = lux != application->raw_lux;
    resume_sample_pending = application->suspend_guard.resume_sample_pending;
    result = handle_sensor_sample(application, lux);
    if (result < 0 || !changed || resume_sample_pending)
        return result;
    now_usec = monotonic_usec();
    application->sensor_average_until_usec = now_usec
        + sensor_average_window_usec(application);
    return schedule_sensor_sample(
        application,
        now_usec + SENSOR_SAMPLE_INTERVAL_USEC
    );

retry:
    application->sensor_refresh_attempts++;
    if (application->sensor_refresh_attempts >= SENSOR_REFRESH_MAX_ATTEMPTS) {
        fprintf(
            stderr,
            "ambient sensor did not recover after %u refresh attempts\n",
            application->sensor_refresh_attempts
        );
        application->sensor_refresh_attempts = 0;
        return 0;
    }
    result = sd_event_now(event, CLOCK_MONOTONIC, &now_usec);
    if (result < 0)
        return result;
    result = sd_event_source_set_time(
        source,
        now_usec + SENSOR_REFRESH_RETRY_USEC
    );
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(source, SD_EVENT_ONESHOT);
}

static int schedule_sensor_refresh(Application *application, uint64_t wakeup_usec)
{
    int result;

    application->sensor_refresh_attempts = 0;
    if (application->sensor_refresh_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->sensor_refresh_timer,
            CLOCK_MONOTONIC,
            wakeup_usec,
            UINT64_C(1000),
            on_sensor_refresh_timer,
            application
        );
    }
    result = sd_event_source_set_time(application->sensor_refresh_timer, wakeup_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(
        application->sensor_refresh_timer,
        SD_EVENT_ONESHOT
    );
}

static int on_sensor_owner_changed(
    sd_bus_message *message,
    void *userdata,
    sd_bus_error *error
)
{
    Application *application = userdata;
    const char *name = NULL;
    const char *old_owner = NULL;
    const char *new_owner = NULL;
    uint64_t wakeup_usec;
    int result;

    (void)error;
    result = sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner);
    if (result < 0)
        return result;
    if (strcmp(name, SENSOR_DESTINATION) != 0)
        return 0;

    if (old_owner[0] != '\0') {
        application->light_claimed = false;
        fprintf(stderr, "ambient sensor proxy disappeared; waiting for replacement\n");
    }
    if (new_owner[0] == '\0' || application->light_claimed)
        return 0;

    wakeup_usec = monotonic_usec() + SENSOR_OWNER_SETTLE_USEC;
    if (application->suspend_guard.resume_sample_pending
        && wakeup_usec < application->suspend_guard.resume_after_usec) {
        wakeup_usec = application->suspend_guard.resume_after_usec;
    }
    result = schedule_sensor_refresh(application, wakeup_usec);
    if (result >= 0)
        fprintf(stderr, "ambient sensor proxy returned; scheduling a fresh claim\n");
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
    char default_profile[PATH_MAX];
    const char *profile_path = application->configuration.calibration_profile;
    double lux = 0.0;
    int brightness = 0;
    int keyboard_brightness = 0;
    bool lid_closed = false;
    SabgUserBrightness saved_brightness;
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
    result = sd_bus_match_signal(
        application->system_bus,
        &application->sensor_owner_slot,
        DBUS_DESTINATION,
        DBUS_PATH,
        DBUS_INTERFACE,
        "NameOwnerChanged",
        on_sensor_owner_changed,
        application
    );
    if (result < 0)
        return result;
    result = sd_bus_match_signal(
        application->session_bus,
        &application->display_power_properties_slot,
        MUTTER_DISPLAY_DESTINATION,
        MUTTER_DISPLAY_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        on_display_power_properties,
        application
    );
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
        if (application->configuration.verbose) {
            fprintf(stderr, "Apple ALS keepalive: %s every %u ms\n",
                application->keepalive.path,
                application->configuration.apple_refresh_ms);
        }
    }

    result = claim_light_sensor(application);
    if (result < 0)
        return result;
    result = get_initial_state(application, &lux, &brightness);
    if (result < 0)
        return result;
    if (get_lid_closed(application, &lid_closed) < 0)
        lid_closed = false;
    result = discover_keyboard_backlight(application, &keyboard_brightness);
    if (result < 0 && result != -EOPNOTSUPP) {
        fprintf(stderr, "Keyboard backlight unavailable; display control remains active\n");
        application->keyboard_backend = KEYBOARD_BACKEND_NONE;
    }
    application->last_user_display_brightness = brightness;
    application->last_user_keyboard_brightness = keyboard_brightness;
    if (configure_user_brightness_path(application)) {
        result = sabg_user_brightness_load(
            application->user_brightness_path,
            &saved_brightness
        );
        if (result == 0) {
            application->last_user_display_brightness = saved_brightness.display;
            application->last_user_keyboard_brightness = saved_brightness.keyboard;
        } else if (result == -ENOENT) {
            save_user_brightness(application);
        } else {
            fprintf(
                stderr,
                "Ignoring invalid user brightness state: %s\n",
                strerror(-result)
            );
        }
    }
    if (get_display_powered_down(application, &application->display_powered_down) < 0)
        application->display_powered_down = false;

    if (application->configuration.check_only || application->configuration.verbose) {
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
    }
    if (application->configuration.check_only)
        return 1;

    now_usec = monotonic_usec();
    application->raw_lux = lux;
    application->current_lux = lux;
    sabg_lux_average_init(
        &application->lux_average,
        sensor_average_window_usec(application)
    );
    sabg_lux_average_reset(&application->lux_average, lux, now_usec);
    sabg_display_response_init(&application->display_response);
    if (!application->configuration.calibration_profile_disabled) {
        if (profile_path == NULL)
            profile_path = default_calibration_profile(default_profile);
        if (profile_path != NULL) {
            result = sabg_display_response_load(&application->display_response, profile_path);
            if (result == 0) {
                if (application->configuration.verbose)
                    fprintf(stderr, "Display response profile: %s\n", profile_path);
            } else if (result != -ENOENT) {
                fprintf(
                    stderr,
                    "Ignoring invalid display response profile %s: %s\n",
                    profile_path,
                    strerror(-result)
                );
            }
        }
    }
    sabg_smoother_init(
        &application->smoother,
        brightness,
        application->configuration.brighten_step_ms,
        application->configuration.dim_step_ms,
        application->configuration.maximum_transition_ms
    );
    sabg_target_hysteresis_init(
        &application->target_hysteresis,
        brightness,
        application->configuration.minimum_percentage,
        application->configuration.maximum_percentage,
        application->configuration.hysteresis_percentage
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
    if (!application->configuration.legacy_transitions) {
        sabg_ambient_model_set_dimming_time_constant(
            &application->ambient,
            application->configuration.ambient_time_constant_seconds
                * TRAJECTORY_DIMMING_TIME_CONSTANT_FACTOR
        );
        sabg_ambient_model_set_dimming_finish_distance(
            &application->ambient,
            TRAJECTORY_DIMMING_FINISH_DISTANCE
        );
        sabg_ambient_model_set_activity_threshold(
            &application->ambient,
            (double)application->configuration.hysteresis_percentage
        );
        sabg_ambient_model_set_large_change_response(
            &application->ambient,
            LARGE_CHANGE_THRESHOLD,
            application->configuration.ambient_time_constant_seconds
                * LARGE_CHANGE_TIME_CONSTANT_FACTOR,
            LARGE_CHANGE_FINISH_DISTANCE
        );
    }
    sabg_trajectory_init(
        &application->display_trajectory,
        sabg_display_response_to_coordinate(
            &application->display_response,
            (double)brightness
        ),
        application->configuration.brighten_step_ms,
        application->configuration.dim_step_ms,
        application->configuration.maximum_transition_ms,
        sabg_display_response_to_coordinate(
            &application->display_response,
            (double)application->configuration.minimum_percentage
        ),
        sabg_display_response_to_coordinate(
            &application->display_response,
            (double)application->configuration.maximum_percentage
        ),
        now_usec
    );
    sabg_output_quantizer_init(
        &application->display_quantizer,
        brightness,
        application->configuration.minimum_percentage,
        application->configuration.maximum_percentage,
        application->configuration.hysteresis_percentage
    );
    sabg_suspend_guard_init(
        &application->suspend_guard,
        lid_closed,
        brightness,
        RESUME_SENSOR_SETTLE_USEC
    );
    if (lid_closed && application->configuration.apple_keepalive) {
        result = sabg_apple_als_keepalive_set_enabled(
            &application->keepalive,
            false
        );
        if (result < 0)
            return result;
    }
    application->model_ready = true;

    if (application->keyboard_backend != KEYBOARD_BACKEND_NONE) {
        sabg_smoother_init(
            &application->keyboard_smoother,
            keyboard_brightness,
            application->configuration.brighten_step_ms,
            application->configuration.dim_step_ms,
            application->configuration.maximum_transition_ms
        );
        sabg_target_hysteresis_init(
            &application->keyboard_target_hysteresis,
            keyboard_brightness,
            0,
            100,
            application->configuration.hysteresis_percentage
        );
        sabg_keyboard_model_init(
            &application->keyboard_model,
            lux,
            keyboard_brightness,
            application->configuration.ambient_time_constant_seconds,
            now_usec
        );
        if (!application->configuration.legacy_transitions) {
            sabg_keyboard_model_set_activity_threshold(
                &application->keyboard_model,
                (double)application->configuration.hysteresis_percentage
            );
            sabg_keyboard_model_set_large_change_response(
                &application->keyboard_model,
                LARGE_CHANGE_THRESHOLD,
                application->configuration.ambient_time_constant_seconds
                    * LARGE_CHANGE_TIME_CONSTANT_FACTOR,
                LARGE_CHANGE_FINISH_DISTANCE
            );
        }
        sabg_trajectory_init(
            &application->keyboard_trajectory,
            keyboard_brightness,
            application->configuration.brighten_step_ms,
            application->configuration.dim_step_ms,
            application->configuration.maximum_transition_ms,
            0,
            100,
            now_usec
        );
        sabg_output_quantizer_init(
            &application->keyboard_quantizer,
            keyboard_brightness,
            0,
            100,
            application->configuration.hysteresis_percentage
        );
        application->keyboard_model_ready = true;
        if (application->display_powered_down) {
            result = write_keyboard_brightness(application, 0);
            if (result < 0)
                return result;
        }
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
        application->system_bus,
        &application->lid_properties_slot,
        LOGIND_DESTINATION,
        LOGIND_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        on_lid_properties,
        application
    );
    if (result < 0)
        return result;
    result = sd_bus_match_signal(
        application->system_bus,
        &application->prepare_for_sleep_slot,
        LOGIND_DESTINATION,
        LOGIND_PATH,
        LOGIND_INTERFACE,
        "PrepareForSleep",
        on_prepare_for_sleep,
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
    application->motion_timer = sd_event_source_unref(application->motion_timer);
    application->resume_restore_timer = sd_event_source_unref(
        application->resume_restore_timer
    );
    application->sensor_refresh_timer = sd_event_source_unref(
        application->sensor_refresh_timer
    );
    application->sensor_sample_timer = sd_event_source_unref(
        application->sensor_sample_timer
    );
    application->sensor_properties_slot = sd_bus_slot_unref(application->sensor_properties_slot);
    application->sensor_owner_slot = sd_bus_slot_unref(application->sensor_owner_slot);
    application->brightness_properties_slot = sd_bus_slot_unref(application->brightness_properties_slot);
    application->keyboard_brightness_slot = sd_bus_slot_unref(application->keyboard_brightness_slot);
    application->display_power_properties_slot = sd_bus_slot_unref(
        application->display_power_properties_slot
    );
    application->lid_properties_slot = sd_bus_slot_unref(application->lid_properties_slot);
    application->prepare_for_sleep_slot = sd_bus_slot_unref(
        application->prepare_for_sleep_slot
    );
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
        if (application.configuration.verbose)
            fprintf(stderr, "smooth ambient brightness active\n");
        result = sd_event_loop(application.event);
    }

    if (result < 0)
        fprintf(stderr, "Event loop failed: %s\n", strerror(-result));
    application_destroy(&application);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
