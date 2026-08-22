// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_als_keepalive.h"
#include "sabg/ambient_model.h"
#include "sabg/smoother.h"

#include <errno.h>
#include <getopt.h>
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

#define VERSION "0.1.0"

#define SENSOR_DESTINATION "net.hadess.SensorProxy"
#define SENSOR_PATH "/net/hadess/SensorProxy"
#define SENSOR_INTERFACE "net.hadess.SensorProxy"

#define GNOME_POWER_DESTINATION "org.gnome.SettingsDaemon.Power"
#define GNOME_POWER_PATH "/org/gnome/SettingsDaemon/Power"
#define GNOME_SCREEN_INTERFACE "org.gnome.SettingsDaemon.Power.Screen"

typedef struct {
    unsigned int brighten_step_ms;
    unsigned int dim_step_ms;
    unsigned int apple_refresh_ms;
    double ambient_time_constant_seconds;
    int minimum_percentage;
    int maximum_percentage;
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
    sd_bus *system_bus;
    sd_bus *session_bus;
    sd_bus_slot *sensor_properties_slot;
    sd_bus_slot *brightness_properties_slot;
    SabgAmbientModel ambient;
    SabgSmoother smoother;
    SabgAppleAlsKeepalive keepalive;
    double current_lux;
    int last_written_percentage;
    bool light_claimed;
    bool model_ready;
    bool write_pending;
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
        "Smooth only GNOME's ambient-light brightness changes.\n"
        "\n"
        "  --brighten-step-ms N       delay per 1%% increase (default: 40)\n"
        "  --dim-step-ms N            delay per 1%% decrease (default: 60)\n"
        "  --ambient-time-constant S  target filter time constant (default: 1.6)\n"
        "  --min-brightness N         automatic floor (default: 2)\n"
        "  --max-brightness N         automatic ceiling (default: 100)\n"
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
        OPTION_AMBIENT_TIME_CONSTANT,
        OPTION_MIN_BRIGHTNESS,
        OPTION_MAX_BRIGHTNESS,
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
        {"ambient-time-constant", required_argument, NULL, OPTION_AMBIENT_TIME_CONSTANT},
        {"min-brightness", required_argument, NULL, OPTION_MIN_BRIGHTNESS},
        {"max-brightness", required_argument, NULL, OPTION_MAX_BRIGHTNESS},
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
        .apple_refresh_ms = 500,
        .ambient_time_constant_seconds = 1.6,
        .minimum_percentage = 2,
        .maximum_percentage = 100,
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

static int schedule_animation(Application *application, uint64_t delay_usec);

static int ensure_animation_scheduled(Application *application)
{
    int enabled;

    if (!sabg_smoother_active(&application->smoother))
        return 0;
    if (application->animation_timer == NULL)
        return schedule_animation(
            application,
            sabg_smoother_next_delay_usec(&application->smoother)
        );

    {
        int result = sd_event_source_get_enabled(application->animation_timer, &enabled);
        if (result < 0)
            return result;
    }
    if (enabled != SD_EVENT_OFF)
        return 0;
    return schedule_animation(
        application,
        sabg_smoother_next_delay_usec(&application->smoother)
    );
}

static int write_brightness(Application *application, int percentage)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result;

    if (application->configuration.dry_run) {
        if (application->configuration.verbose)
            fprintf(stderr, "dry-run brightness: %d%%\n", percentage);
        return 0;
    }

    application->last_written_percentage = percentage;
    application->write_pending = true;
    result = sd_bus_set_property(
        application->session_bus,
        GNOME_POWER_DESTINATION,
        GNOME_POWER_PATH,
        GNOME_SCREEN_INTERFACE,
        "Brightness",
        &error,
        "i",
        (int32_t)percentage
    );
    if (result < 0) {
        application->write_pending = false;
        fprintf(stderr, "Unable to set GNOME brightness: %s\n",
            error.message != NULL ? error.message : strerror(-result));
    }
    sd_bus_error_free(&error);
    return result;
}

static int on_animation_timer(sd_event_source *source, uint64_t usec, void *userdata)
{
    Application *application = userdata;
    int percentage;
    int result;

    (void)source;
    (void)usec;
    percentage = sabg_smoother_advance(&application->smoother);
    result = write_brightness(application, percentage);
    if (result < 0)
        return result;

    if (sabg_smoother_active(&application->smoother))
        return schedule_animation(
            application,
            sabg_smoother_next_delay_usec(&application->smoother)
        );

    if (application->configuration.verbose)
        fprintf(stderr, "ambient transition complete at %d%%\n", percentage);
    return 0;
}

static int schedule_animation(Application *application, uint64_t delay_usec)
{
    uint64_t now;
    int result;

    result = sd_event_now(application->event, CLOCK_MONOTONIC, &now);
    if (result < 0)
        return result;

    if (application->animation_timer == NULL) {
        return sd_event_add_time(
            application->event,
            &application->animation_timer,
            CLOCK_MONOTONIC,
            now + delay_usec,
            0,
            on_animation_timer,
            application
        );
    }

    result = sd_event_source_set_time(application->animation_timer, now + delay_usec);
    if (result < 0)
        return result;
    return sd_event_source_set_enabled(application->animation_timer, SD_EVENT_ONESHOT);
}

static int on_sensor_properties(sd_bus_message *message, void *userdata, sd_bus_error *error)
{
    Application *application = userdata;
    double lux = 0.0;
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

    target = sabg_ambient_model_observe(&application->ambient, lux, monotonic_usec());
    if (application->configuration.verbose)
        fprintf(stderr, "ambient %.2f lux -> target %d%%\n", lux, target);

    if (sabg_smoother_set_target(&application->smoother, target)) {
        result = ensure_animation_scheduled(application);
        if (result < 0)
            return result;
    }
    return 0;
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

    if (application->write_pending && percentage == application->last_written_percentage) {
        application->write_pending = false;
        return 0;
    }

    if (!application->model_ready)
        return 0;

    application->write_pending = false;
    application->smoother.current = percentage;
    application->smoother.target = percentage;
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

    printf("SensorProxy: %.2f lux\n", lux);
    printf("GNOME brightness: %d%%\n", brightness);
    if (application->configuration.check_only)
        return 1;

    application->current_lux = lux;
    sabg_smoother_init(
        &application->smoother,
        brightness,
        application->configuration.brighten_step_ms,
        application->configuration.dim_step_ms
    );
    sabg_ambient_model_init(
        &application->ambient,
        lux,
        brightness,
        application->configuration.ambient_time_constant_seconds,
        application->configuration.minimum_percentage,
        application->configuration.maximum_percentage,
        monotonic_usec()
    );
    application->model_ready = true;

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

    return 0;
}

static void application_destroy(Application *application)
{
    release_light_sensor(application);
    sabg_apple_als_keepalive_destroy(&application->keepalive);
    application->animation_timer = sd_event_source_unref(application->animation_timer);
    application->sensor_properties_slot = sd_bus_slot_unref(application->sensor_properties_slot);
    application->brightness_properties_slot = sd_bus_slot_unref(application->brightness_properties_slot);
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
