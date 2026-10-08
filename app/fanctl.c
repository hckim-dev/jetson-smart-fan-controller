/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "smartfan_uapi.h"
#include "encoder.h"
#include "lcd.h"
#include "sensor.h"

struct backend
{
    int fd;
    bool dry_run;
    struct smartfan_status state;
    uint64_t lease_deadline;
    uint64_t on_deadline;
    uint64_t boost_deadline;
    uint32_t level;
    uint32_t last_nonzero;
    bool legacy;
    bool led_test;
    uint32_t led_count;
    struct lcd_display *lcd;
    struct sensor_reader sensor;
    struct sensor_sample sample;
    bool sensor_enabled, have_sample, auto_mode, auto_armed;
    bool auto_rearm_pending, auto_inhibited;
    unsigned int auto_target;
};

static volatile sig_atomic_t received_signal;
static int wake_write_fd = -1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) < 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static bool sensor_fresh(const struct backend *backend)
{
    uint64_t now = now_ms();
    return backend->have_sample && !backend->sample.error &&
           now >= backend->sample.timestamp_ms &&
           now - backend->sample.timestamp_ms < SENSOR_MAX_AGE_MS;
}

static void print_environment(const struct backend *backend)
{
    printf("MODE %s armed=%u target=%u\n", backend->auto_mode ? "AUTO" : "MANUAL",
           backend->auto_armed, backend->auto_target);
    if (!backend->sensor_enabled)
        return;
    if (!backend->have_sample)
        puts("SENSOR pending");
    else if (backend->sample.error)
        printf("SENSOR error=%s\n", strerror(backend->sample.error));
    else if (!sensor_fresh(backend))
        puts("SENSOR stale");
    else
        printf("SENSOR temperature=%.1fC pressure=%.2fhPa age_ms=%" PRIu64 "\n",
               backend->sample.temperature_mc / 1000.0, backend->sample.pressure_pa / 100.0,
               now_ms() - backend->sample.timestamp_ms);
}

static void handle_signal(int signo)
{
    int saved_errno = errno;
    unsigned char byte = (unsigned char)signo;
    received_signal = signo;
    if (wake_write_fd >= 0)
    {
        ssize_t result = write(wake_write_fd, &byte, sizeof(byte));
        (void)result;
    }
    errno = saved_errno;
}

static const char *reason_name(uint32_t reason)
{
    switch (reason)
    {
    case SMARTFAN_STOP_INITIAL:
        return "initial";
    case SMARTFAN_STOP_USER:
        return "user";
    case SMARTFAN_STOP_CLOSE:
        return "close";
    case SMARTFAN_STOP_LEASE:
        return "lease";
    case SMARTFAN_STOP_MAX_ON:
        return "max_on";
    case SMARTFAN_STOP_PWM_ERROR:
        return "pwm_error";
    case SMARTFAN_STOP_REMOVE:
        return "remove";
    case SMARTFAN_STOP_SUSPEND:
        return "suspend";
    case SMARTFAN_STOP_SENSOR:
        return "sensor";
    default:
        return "unknown";
    }
}

static void dry_expire(struct backend *backend, uint64_t now)
{
    if (!backend->state.running)
        return;
    if (now >= backend->on_deadline)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_MAX_ON;
    }
    else if (now >= backend->lease_deadline)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_LEASE;
    }
    if (!backend->state.running)
    {
        backend->led_test = false;
        backend->auto_inhibited = true;
    }
}

static int backend_get(struct backend *backend, struct smartfan_status *state)
{
    if (!backend->dry_run)
    {
        memset(state, 0, sizeof(*state));
        if (ioctl(backend->fd, SMARTFAN_IOC_GET, state) < 0)
            return -1;
        if (state->abi_version != SMARTFAN_ABI_VERSION)
        {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    uint64_t now = now_ms();
    dry_expire(backend, now);
    *state = backend->state;
    state->lease_remaining_ms = state->running ? (uint32_t)(backend->lease_deadline - now) : 0;
    state->on_remaining_ms = state->running ? (uint32_t)(backend->on_deadline - now) : 0;
    return 0;
}

static int backend_set(struct backend *backend, bool enabled)
{
    struct smartfan_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .enabled = enabled ? 1U : 0U,
        .reserved = {0, 0},
    };
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_SET, &request);
    uint64_t now = now_ms();
    dry_expire(backend, now);
    backend->led_test = false;
    if (!enabled)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_USER;
    }
    else
    {
        if (!backend->state.running)
        {
            if (!backend->level)
                backend->level = backend->last_nonzero;
            backend->on_deadline = now + backend->state.max_on_ms;
            backend->boost_deadline = backend->level < SMARTFAN_MAX_SPEED_LEVEL
                                          ? now + SMARTFAN_DEFAULT_BOOST_MS
                                          : 0;
        }
        backend->state.running = 1;
        backend->auto_inhibited = false;
        backend->lease_deadline = now + backend->state.lease_ms;
    }
    return 0;
}

static int backend_heartbeat(struct backend *backend)
{
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_HEARTBEAT, 0UL);
    uint64_t now = now_ms();
    dry_expire(backend, now);
    if (!backend->state.running)
    {
        errno = backend->state.stop_reason == SMARTFAN_STOP_LEASE ||
                        backend->state.stop_reason == SMARTFAN_STOP_MAX_ON
                    ? ETIMEDOUT
                    : EPIPE;
        return -1;
    }
    backend->lease_deadline = now + backend->state.lease_ms;
    return 0;
}

static int backend_get_speed(struct backend *backend, struct smartfan_speed_status *speed)
{
    memset(speed, 0, sizeof(*speed));
    if (!backend->dry_run && !backend->legacy)
    {
        if (ioctl(backend->fd, SMARTFAN_IOC_GET_SPEED, speed) == 0)
        {
            if (speed->abi_version != SMARTFAN_ABI_VERSION ||
                speed->max_level != SMARTFAN_MAX_SPEED_LEVEL ||
                speed->level > speed->max_level || speed->duty_percent > 100)
            {
                errno = EPROTO;
                return -1;
            }
            backend->level = speed->level;
            return 0;
        }
        if (errno != ENOTTY)
            return -1;
        backend->legacy = true;
    }
    struct smartfan_status state;
    if (backend_get(backend, &state) < 0)
        return -1;
    uint64_t now = now_ms();
    speed->abi_version = SMARTFAN_ABI_VERSION;
    speed->level = backend->level;
    speed->max_level = SMARTFAN_MAX_SPEED_LEVEL;
    if (backend->dry_run)
    {
        speed->capabilities = SMARTFAN_CAP_PWM | SMARTFAN_CAP_LED_BAR | SMARTFAN_CAP_AUTO;
        speed->period_ns = SMARTFAN_DEFAULT_PWM_PERIOD_NS;
        if (state.running && now < backend->boost_deadline)
            speed->boost_remaining_ms = (uint32_t)(backend->boost_deadline - now);
    }
    speed->duty_percent = !state.running ? 0 : speed->boost_remaining_ms ? 100
                                                                         : smartfan_level_duty(backend->level);
    return 0;
}

static int backend_set_speed(struct backend *backend, uint32_t level)
{
    struct smartfan_speed_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .level = level,
    };
    if (!backend->dry_run)
    {
        if (backend->legacy)
        {
            errno = EOPNOTSUPP;
            return -1;
        }
        return ioctl(backend->fd, SMARTFAN_IOC_SET_SPEED, &request);
    }
    dry_expire(backend, now_ms());
    backend->led_test = false;
    backend->level = level;
    if (!level)
        return backend_set(backend, false);
    backend->last_nonzero = level;
    if (backend->state.running)
        backend->lease_deadline = now_ms() + backend->state.lease_ms;
    return 0;
}

static int backend_get_leds(struct backend *backend, struct smartfan_led_status *leds)
{
    memset(leds, 0, sizeof(*leds));
    if (!backend->dry_run)
    {
        if (ioctl(backend->fd, SMARTFAN_IOC_GET_LEDS, leds) < 0)
        {
            if (errno != ENOTTY)
                return -1;
            leds->abi_version = SMARTFAN_ABI_VERSION;
            return 0;
        }
        if (leds->abi_version != SMARTFAN_ABI_VERSION ||
            leds->mode > SMARTFAN_LED_TEST || leds->count > SMARTFAN_LED_SEGMENTS ||
            leds->available > 1)
        {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    dry_expire(backend, now_ms());
    leds->abi_version = SMARTFAN_ABI_VERSION;
    leds->available = 1;
    leds->mode = backend->led_test ? SMARTFAN_LED_TEST : SMARTFAN_LED_AUTO;
    leds->count = backend->led_test ? backend->led_count : backend->state.running ? smartfan_level_leds(backend->level)
                                                                                  : 0;
    return 0;
}

static int backend_set_leds(struct backend *backend, bool test, uint32_t count)
{
    struct smartfan_led_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .mode = test ? SMARTFAN_LED_TEST : SMARTFAN_LED_AUTO,
        .count = count,
    };
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_SET_LEDS, &request);
    dry_expire(backend, now_ms());
    if (test && backend->state.running)
    {
        errno = EBUSY;
        return -1;
    }
    backend->led_test = test;
    backend->led_count = count;
    return 0;
}

static int print_speed(struct backend *backend)
{
    struct smartfan_speed_status speed;
    if (backend_get_speed(backend, &speed) < 0)
        return -1;
    printf("SPEED level=%" PRIu32 "/%" PRIu32 " duty=%" PRIu32
           "%% pwm=%u period_ns=%" PRIu32 " boost_remaining_ms=%" PRIu32 "\n",
           (uint32_t)speed.level, (uint32_t)speed.max_level,
           (uint32_t)speed.duty_percent, !!(speed.capabilities & SMARTFAN_CAP_PWM),
           (uint32_t)speed.period_ns, (uint32_t)speed.boost_remaining_ms);
    struct smartfan_led_status leds;
    if (backend_get_leds(backend, &leds) < 0)
        return -1;
    printf("LEDBAR available=%u mode=%s count=%u/8\n", (unsigned int)leds.available,
           leds.mode == SMARTFAN_LED_TEST ? "test" : "auto", (unsigned int)leds.count);
    print_environment(backend);
    if (backend->lcd)
    {
        struct smartfan_status state;
        if (backend_get(backend, &state) < 0)
            return -1;
        lcd_publish_environment(backend->lcd, state.running != 0, speed.level,
                                !state.running && leds.mode == SMARTFAN_LED_AUTO ? 0 : leds.count,
                                backend->auto_mode, backend->sensor_enabled, sensor_fresh(backend),
                                backend->sample.temperature_mc);
    }
    return 0;
}

static int backend_auto_update(struct backend *backend, uint32_t level, bool rearm)
{
    struct smartfan_auto_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .level = level,
        .rearm = rearm,
        .sample_boottime_ms = backend->sample.timestamp_ms,
    };
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_SET_AUTO, &request);
    uint64_t now = now_ms();
    dry_expire(backend, now);
    if (!smartfan_auto_sample_fresh(now, request.sample_boottime_ms))
    {
        backend_set(backend, false);
        backend->auto_inhibited = true;
        errno = ESTALE;
        return -1;
    }
    if (!smartfan_auto_rearm_allowed(backend->auto_inhibited, request.rearm))
    {
        errno = ECANCELED;
        return -1;
    }
    backend->auto_inhibited = false;
    if (backend_set_speed(backend, level) < 0)
        return -1;
    return level ? backend_set(backend, true) : 0;
}

/* Kernel validates AUTO inhibition and sample freshness atomically with output. */
static int auto_tick(struct backend *backend)
{
    if (!backend->auto_mode)
        return 0;
    if (!sensor_fresh(backend))
    {
        if (backend->auto_armed)
        {
            backend->auto_armed = false;
            backend->auto_rearm_pending = false;
            if (backend_set(backend, false) < 0)
                return -1;
            puts("AUTO STOP reason=sensor_fault_or_stale; explicit ON required");
            return print_speed(backend);
        }
        return 0;
    }
    struct smartfan_status state;
    struct smartfan_speed_status speed;
    if (backend_get(backend, &state) < 0 || backend_get_speed(backend, &speed) < 0)
        return -1;
    if (backend->auto_armed && !backend->auto_rearm_pending && !state.running &&
        (state.stop_reason == SMARTFAN_STOP_MAX_ON || state.stop_reason == SMARTFAN_STOP_LEASE ||
         state.stop_reason == SMARTFAN_STOP_PWM_ERROR || state.stop_reason == SMARTFAN_STOP_SUSPEND ||
         state.stop_reason == SMARTFAN_STOP_SENSOR))
    {
        backend->auto_armed = false;
        puts("AUTO STOP reason=driver_limit; explicit ON required");
        return 0;
    }
    backend->auto_target = auto_temperature_level(backend->auto_target, backend->sample.temperature_mc);
    bool changed = speed.level != backend->auto_target;
    if (backend->auto_armed && (changed || (backend->auto_target && !state.running) || backend->auto_rearm_pending))
    {
        bool rearm = backend->auto_rearm_pending;
        backend->auto_rearm_pending = false;
        if (backend_auto_update(backend, backend->auto_target, rearm) < 0)
        {
            if (errno == ECANCELED || errno == ESTALE || errno == EHOSTDOWN)
            {
                backend->auto_armed = false;
                puts("AUTO STOP reason=atomic_guard; explicit ON required");
                return 0;
            }
            return -1;
        }
        if (backend->auto_target && !state.running)
        {
            puts("AUTO RUN: fresh temperature requested a nonzero level");
            changed = true;
        }
    }
    else if (!backend->auto_armed && changed && backend_set_speed(backend, backend->auto_target) < 0)
        return -1;
    return changed ? print_speed(backend) : 0;
}

static int change_level(struct backend *backend, int delta)
{
    struct smartfan_speed_status speed;
    if (backend_get_speed(backend, &speed) < 0)
        return -1;
    return backend_set_speed(backend, encoder_clamp_level(speed.level, delta, speed.max_level));
}

static void print_state(const struct smartfan_status *state)
{
    if (state->running)
    {
        printf("STATE ON lease_remaining_ms=%" PRIu32
               " on_remaining_ms=%" PRIu32 "\n",
               (uint32_t)state->lease_remaining_ms,
               (uint32_t)state->on_remaining_ms);
    }
    else
    {
        printf("STATE OFF reason=%s\n", reason_name(state->stop_reason));
    }
}

static void usage(FILE *out, const char *program)
{
    fprintf(out, "Usage: %s [--dry-run | --device PATH]\n"
                 "       [--lease-ms N --max-on-ms N] (dry-run only)\n"
                 "Commands: on, off, status, speed 0..5, up, down, led 0..8, led auto, heartbeat, help, quit\n"
                 "       [--encoder [--encoder-chip PATH] [--reverse-encoder]]\n"
                 "       [--lcd --lcd-address 0x27 [--lcd-bus /dev/i2c-7]]\n"
                 "       [--bmp180 [--sensor-bus /dev/i2c-7]]\n"
                 "Sensor commands: mode manual, mode auto; temp N/error (dry-run only).\n"
                 "Dry-run encoder simulation: cw, ccw (requires --encoder).\n"
                 "Keep this foreground process open while the fan is ON.\n",
            program);
}

static int parse_ms(const char *text, uint32_t minimum, uint32_t maximum,
                    uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (!text[0] || text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed < minimum || parsed > maximum)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

/* Return 1 for quit, -1 for a backend error, 0 to continue. */
static int command(struct backend *backend, char *line, bool *known_running,
                   bool encoder_enabled, bool reverse_encoder)
{
    struct smartfan_status state;
    if (received_signal)
        return 1;
    while (*line == ' ' || *line == '\t')
        ++line;
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\r' ||
                      line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
    if (!length)
        return 0;
    if (!strcmp(line, "quit"))
        return 1;
    if (!strcmp(line, "help"))
    {
        puts("Commands: on, off, status, speed 0..5, up, down, led 0..8, led auto, mode manual, mode auto, heartbeat, help, quit\nDry-run sensor: temp N, temp error");
        return 0;
    }
    if (!strcmp(line, "mode auto") || !strcmp(line, "mode manual"))
    {
        bool automatic = !strcmp(line, "mode auto");
        struct smartfan_speed_status speed;
        if (automatic && (!backend->sensor_enabled ||
                          backend_get_speed(backend, &speed) < 0 || !(speed.capabilities & SMARTFAN_CAP_AUTO)))
        {
            fputs("ERR AUTO requires --bmp180 and an updated atomic-AUTO driver\n", stderr);
            return 0;
        }
        if (backend_set(backend, false) < 0)
            return -1;
        backend->auto_mode = automatic;
        backend->auto_armed = false;
        backend->auto_rearm_pending = false;
        backend->auto_target = 0;
    }
    else if (!strncmp(line, "temp ", 5))
    {
        if (!backend->dry_run || !backend->sensor_enabled)
        {
            fputs("ERR temp injection requires --dry-run --bmp180\n", stderr);
            return 0;
        }
        int32_t temperature = 0;
        bool fault = !strcmp(line + 5, "error");
        if (!fault)
        {
            char *end;
            errno = 0;
            double value = strtod(line + 5, &end);
            if (errno || end == line + 5 || *end || !isfinite(value) || value < -40 || value > 85)
            {
                fputs("ERR temp must be -40..85 Celsius or error\n", stderr);
                return 0;
            }
            temperature = (int32_t)(value * 1000 + (value >= 0 ? 0.5 : -0.5));
        }
        backend->sample = (struct sensor_sample){.magic = SENSOR_MAGIC, .sequence = 1, .error = fault ? EIO : 0, .temperature_mc = temperature, .pressure_pa = 101325, .timestamp_ms = now_ms()};
        backend->have_sample = true;
    }
    else if (backend->auto_mode && (!strncmp(line, "speed ", 6) ||
                                    !strcmp(line, "up") || !strcmp(line, "down") ||
                                    (!strncmp(line, "led ", 4) && strcmp(line, "led auto")) ||
                                    !strcmp(line, "cw") || !strcmp(line, "ccw")))
    {
        fputs("ERR manual speed/LED controls require mode manual\n", stderr);
        return 0;
    }
    else if (!strcmp(line, "on"))
    {
        if (received_signal)
            return 1;
        if (backend->auto_mode)
        {
            if (!sensor_fresh(backend))
            {
                fputs("ERR AUTO ON requires a fresh valid sensor sample\n", stderr);
                return 0;
            }
            backend->auto_armed = true;
            backend->auto_rearm_pending = true;
        }
        else if (backend_set(backend, true) < 0)
            return -1;
    }
    else if (!strcmp(line, "off"))
    {
        backend->auto_armed = false;
        backend->auto_rearm_pending = false;
        if (backend_set(backend, false) < 0)
            return -1;
    }
    else if (!strcmp(line, "heartbeat"))
    {
        if (backend_heartbeat(backend) < 0 &&
            errno != EPIPE && errno != ETIMEDOUT)
            return -1;
    }
    else if (!strncmp(line, "speed ", 6))
    {
        uint32_t level;
        if (parse_ms(line + 6, 0, SMARTFAN_MAX_SPEED_LEVEL, &level))
        {
            fputs("ERR speed must be an integer 0..5\n", stderr);
            return 0;
        }
        if (backend_set_speed(backend, level) < 0)
            return -1;
    }
    else if (!strncmp(line, "led ", 4))
    {
        uint32_t count = 0;
        bool test = strcmp(line + 4, "auto") != 0;
        if (test && parse_ms(line + 4, 0, SMARTFAN_LED_SEGMENTS, &count))
        {
            fputs("ERR led must be an integer 0..8 or auto\n", stderr);
            return 0;
        }
        if (backend_set_leds(backend, test, count) < 0)
        {
            if (errno == EBUSY || errno == EOPNOTSUPP || errno == ENOTTY)
            {
                fprintf(stderr, "ERR LED Bar: %s (test requires motor OFF and LED DT/driver)\n", strerror(errno));
                return 0;
            }
            return -1;
        }
    }
    else if (!strcmp(line, "up") || !strcmp(line, "down"))
    {
        if (change_level(backend, !strcmp(line, "up") ? 1 : -1) < 0)
            return -1;
    }
    else if (backend->dry_run && encoder_enabled &&
             (!strcmp(line, "cw") || !strcmp(line, "ccw")))
    {
        int delta = !strcmp(line, "cw") ? 1 : -1;
        if (change_level(backend, reverse_encoder ? -delta : delta) < 0)
            return -1;
    }
    else if (strcmp(line, "status"))
    {
        fprintf(stderr, "ERR unknown command: %s\n", line);
        return 0;
    }
    if (auto_tick(backend) < 0 || backend_get(backend, &state) < 0)
        return -1;
    *known_running = state.running != 0;
    print_state(&state);
    return print_speed(backend);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"dry-run", no_argument, NULL, 'n'},
        {"device", required_argument, NULL, 'd'},
        {"lease-ms", required_argument, NULL, 'l'},
        {"max-on-ms", required_argument, NULL, 'm'},
        {"encoder", no_argument, NULL, 'e'},
        {"encoder-chip", required_argument, NULL, 'c'},
        {"reverse-encoder", no_argument, NULL, 'r'},
        {"lcd", no_argument, NULL, 'L'},
        {"lcd-bus", required_argument, NULL, 'B'},
        {"lcd-address", required_argument, NULL, 'A'},
        {"bmp180", no_argument, NULL, 'S'},
        {"sensor-bus", required_argument, NULL, 'I'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct backend backend = {
        .fd = -1,
        .level = SMARTFAN_MAX_SPEED_LEVEL,
        .last_nonzero = SMARTFAN_MAX_SPEED_LEVEL,
        .sensor = {.fd = -1},
        .auto_inhibited = true,
        .state = {
            .abi_version = SMARTFAN_ABI_VERSION,
            .lease_ms = SMARTFAN_DEFAULT_LEASE_MS,
            .max_on_ms = SMARTFAN_DEFAULT_MAX_ON_MS,
            .stop_reason = SMARTFAN_STOP_INITIAL,
        },
    };
    struct smartfan_status state;
    struct encoder_input encoder = {.fd = -1};
    bool encoder_enabled = false, reverse_encoder = false;
    bool lcd_enabled = false, lcd_bus_selected = false, lcd_address_selected = false;
    bool lcd_warned = false;
    const char *lcd_bus = "/dev/i2c-7";
    unsigned int lcd_address = 0x27;
    const char *sensor_bus = "/dev/i2c-7";
    bool sensor_bus_selected = false;
    const char *encoder_chip = NULL;
    struct sigaction action = {0};
    const char *device = "/dev/smartfan";
    const char *exit_reason = "eof";
    bool device_selected = false, timeout_selected = false;
    bool known_running = false, done = false, discarding = false;
    int wake_pipe[2], option, result = EXIT_SUCCESS;
    uint64_t next_heartbeat = 0;
    uint32_t heartbeat_interval;
    char line[64];
    size_t used = 0;

    _Static_assert(sizeof(struct smartfan_request) == 16, "SET ABI size");
    _Static_assert(sizeof(struct smartfan_status) == 32, "GET ABI size");
    _Static_assert(sizeof(struct smartfan_speed_request) == 16, "SET_SPEED ABI size");
    _Static_assert(sizeof(struct smartfan_speed_status) == 32, "GET_SPEED ABI size");
    _Static_assert(sizeof(struct smartfan_auto_request) == 24, "SET_AUTO ABI size");
    _Static_assert(SENSOR_MAX_AGE_MS == SMARTFAN_AUTO_MAX_AGE_MS, "sensor/kernel age policy");
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1)
    {
        switch (option)
        {
        case 'n':
            backend.dry_run = true;
            break;
        case 'd':
            device = optarg;
            device_selected = true;
            break;
        case 'l':
            timeout_selected = true;
            if (parse_ms(optarg, 500U, 10000U, &backend.state.lease_ms))
                goto invalid_options;
            break;
        case 'm':
            timeout_selected = true;
            if (parse_ms(optarg, 1000U, 120000U, &backend.state.max_on_ms))
                goto invalid_options;
            break;
        case 'e':
            encoder_enabled = true;
            break;
        case 'c':
            encoder_chip = optarg;
            break;
        case 'r':
            reverse_encoder = true;
            break;
        case 'L':
            lcd_enabled = true;
            break;
        case 'S':
            backend.sensor_enabled = true;
            break;
        case 'I':
            sensor_bus = optarg;
            sensor_bus_selected = true;
            break;
        case 'B':
            lcd_bus = optarg;
            lcd_bus_selected = true;
            break;
        case 'A':
        {
            char *end;
            errno = 0;
            if (optarg[0] < '0' || optarg[0] > '9')
                goto invalid_options;
            unsigned long address = strtoul(optarg, &end, 0);
            if (errno || *end || !((address >= 0x20 && address <= 0x27) || (address >= 0x38 && address <= 0x3f)))
                goto invalid_options;
            lcd_address = (unsigned int)address;
            lcd_address_selected = true;
            break;
        }
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            goto invalid_options;
        }
    }
    if (optind != argc || (backend.dry_run && device_selected) ||
        (!backend.dry_run && timeout_selected) ||
        backend.state.lease_ms > backend.state.max_on_ms ||
        (!encoder_enabled && (encoder_chip || reverse_encoder)) ||
        (backend.dry_run && encoder_chip) ||
        (!lcd_enabled && (lcd_bus_selected || lcd_address_selected)) ||
        (!backend.sensor_enabled && sensor_bus_selected) ||
        (backend.sensor_enabled && (strlen(sensor_bus) >= 128 || !sensor_bus[0])) ||
        (lcd_enabled && (strlen(lcd_bus) >= 128 || !lcd_bus[0] ||
                         (!backend.dry_run && !lcd_address_selected))))
        goto invalid_options;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
    {
        perror("pipe2");
        return EXIT_FAILURE;
    }
    wake_write_fd = wake_pipe[1];
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0)
    {
        perror("sigaction");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (backend.dry_run)
    {
        puts("DRY-RUN: hardware access disabled");
    }
    else
    {
        backend.fd = open(device, O_RDWR | O_CLOEXEC);
        if (backend.fd < 0)
        {
            perror(device);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("DEVICE %s\n", device);
    }
    if (backend_get(&backend, &state) < 0)
    {
        perror("get status");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (backend.sensor_enabled && !backend.dry_run)
    {
        char program[PATH_MAX];
        ssize_t length = readlink("/proc/self/exe", program, sizeof(program) - 1);
        if (length < 0 || (size_t)length == sizeof(program) - 1)
        {
            perror("resolve sensor program");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        program[length] = '\0';
        char *slash = strrchr(program, '/');
        const char *name = "bmp180-monitor";
        if (!slash || (size_t)(slash + 1 - program) + strlen(name) >= sizeof(program))
        {
            fputs("ERR sensor program path too long\n", stderr);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        strcpy(slash + 1, name);
        if (sensor_start(&backend.sensor, program, sensor_bus) < 0)
        {
            perror("start sensor process");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("SENSOR_PROCESS pid=%ld bus=%s address=0x77\n", (long)backend.sensor.pid, sensor_bus);
    }
    if (lcd_enabled)
    {
        if (lcd_start(&backend.lcd, lcd_bus, lcd_address, backend.dry_run) < 0)
        {
            perror("start LCD worker");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("LCD_CONFIG bus=%s address=0x%02x mapping=RS0/RW1/E2/BL3/D4..7\n",
               lcd_bus, lcd_address);
    }
    if (print_speed(&backend) < 0)
    {
        perror("get speed");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (encoder_enabled && !backend.dry_run)
    {
        struct smartfan_speed_status speed;
        if (backend_get_speed(&backend, &speed) < 0 ||
            !(speed.capabilities & SMARTFAN_CAP_PWM))
        {
            fputs("ERR encoder requires stage 2 PWM DT and driver\n", stderr);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        if (encoder_open(&encoder, encoder_chip, reverse_encoder) < 0)
        {
            perror("open encoder");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("ENCODER %s S1=%u S2=%u reverse=%u\n", encoder.chip_path,
               ENCODER_S1_OFFSET, ENCODER_S2_OFFSET, reverse_encoder);
    }
    heartbeat_interval = state.lease_ms / 4U;
    if (heartbeat_interval > 250U)
        heartbeat_interval = 250U;
    if (heartbeat_interval < 50U)
        heartbeat_interval = 50U;
    printf("READY lease_ms=%" PRIu32 " max_on_ms=%" PRIu32 "\n",
           (uint32_t)state.lease_ms, (uint32_t)state.max_on_ms);
    print_state(&state);
    known_running = state.running != 0;

    while (!done && !received_signal)
    {
        struct pollfd descriptors[4] = {
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = wake_pipe[0], .events = POLLIN},
            {.fd = encoder.fd, .events = POLLIN},
            {.fd = backend.sensor.fd, .events = POLLIN},
        };
        uint64_t now = now_ms();
        sensor_reap(&backend.sensor);
        if (backend_get(&backend, &state) < 0)
        {
            perror("get status");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (known_running && !state.running)
            print_state(&state);
        known_running = state.running != 0;
        if (auto_tick(&backend) < 0)
        {
            perror("AUTO control");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (state.running && now >= next_heartbeat)
        {
            if (backend_heartbeat(&backend) < 0)
            {
                if (errno != EPIPE && errno != ETIMEDOUT)
                {
                    perror("heartbeat");
                    result = EXIT_FAILURE;
                    exit_reason = "error";
                    break;
                }
            }
            next_heartbeat = now_ms() + heartbeat_interval;
        }
        if (backend.lcd)
        {
            struct smartfan_speed_status speed;
            struct smartfan_led_status leds;
            if (backend_get_speed(&backend, &speed) < 0 ||
                backend_get_leds(&backend, &leds) < 0 ||
                backend_get(&backend, &state) < 0)
            {
                perror("get LCD status");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            lcd_publish_environment(backend.lcd, state.running != 0, speed.level,
                                    !state.running && leds.mode == SMARTFAN_LED_AUTO ? 0 : leds.count,
                                    backend.auto_mode, backend.sensor_enabled, sensor_fresh(&backend),
                                    backend.sample.temperature_mc);
            int error = lcd_error(backend.lcd);
            if (error && !lcd_warned)
            {
                fprintf(stderr, "WARN LCD disabled: %s; motor control continues\n", strerror(error));
                if (error == EOPNOTSUPP && !strcmp(lcd_bus, "/dev/i2c-7"))
                    fputs("LCD requires header I2C at 100kHz: install the prepared LCD DT and reboot.\n", stderr);
                lcd_warned = true;
            }
        }
        int ready = poll(descriptors, 4, 50);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (received_signal || descriptors[1].revents)
            break;
        if (descriptors[3].revents)
        {
            for (unsigned int batch = 0; batch < 8; ++batch)
            {
                struct sensor_sample sample;
                int response = sensor_next(&backend.sensor, &sample, now_ms());
                if (!response)
                    break;
                if (response < 0)
                {
                    if (errno != EPIPE || !backend.have_sample || !backend.sample.error)
                        backend.sample.error = errno;
                    backend.have_sample = true;
                    close(backend.sensor.fd);
                    backend.sensor.fd = -1;
                    print_environment(&backend);
                    break;
                }
                backend.sample = sample;
                backend.have_sample = true;
                print_environment(&backend);
            }
            if (auto_tick(&backend) < 0)
            {
                perror("AUTO sensor event");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
        }
        if (descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fputs("ERR encoder unavailable\n", stderr);
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (descriptors[2].revents & POLLIN)
        {
            int delta;
            uint64_t gaps = encoder.sequence_gaps;
            if (encoder_read(&encoder, &delta) < 0)
            {
                perror("encoder");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            if (encoder.sequence_gaps != gaps)
                fputs("WARN encoder event gap: discarded partial cycle and resynchronized\n", stderr);
            /* Apply every detent in order: a net-zero batch can cross STOP. */
            for (unsigned int i = 0; i < encoder.num_steps && !received_signal; ++i)
            {
                if (backend.auto_mode)
                    continue;
                if (change_level(&backend, encoder.steps[i]) < 0 || print_speed(&backend) < 0)
                {
                    perror("encoder speed");
                    result = EXIT_FAILURE;
                    exit_reason = "error";
                    done = true;
                    break;
                }
            }
            if (done || received_signal)
                break;
        }
        if (descriptors[0].revents & (POLLERR | POLLNVAL))
        {
            fprintf(stderr, "ERR stdin unavailable\n");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP))
        {
            char bytes[128];
            ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                perror("read stdin");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            if (!count)
                break;
            for (ssize_t i = 0; i < count && !done && !received_signal; ++i)
            {
                if (bytes[i] == '\n')
                {
                    if (discarding)
                    {
                        fprintf(stderr, "ERR command too long or contains NUL\n");
                    }
                    else
                    {
                        line[used] = '\0';
                        int response = command(&backend, line, &known_running,
                                               encoder_enabled, reverse_encoder);
                        if (response == 1)
                        {
                            done = true;
                            exit_reason = "quit";
                        }
                        else if (response < 0)
                        {
                            perror("command");
                            done = true;
                            result = EXIT_FAILURE;
                            exit_reason = "error";
                        }
                    }
                    used = 0;
                    discarding = false;
                }
                else if (!discarding)
                {
                    if (bytes[i] == '\0' || used == sizeof(line) - 1U)
                        discarding = true;
                    else
                        line[used++] = bytes[i];
                }
            }
        }
    }
    if (received_signal)
        exit_reason = "signal";
    if (backend_set(&backend, false) < 0)
    {
        perror("request OFF");
        result = EXIT_FAILURE;
    }
    else
    {
        printf("CLOSED state=OFF reason=%s\n", exit_reason);
    }
cleanup:
    encoder_close(&encoder);
    if (backend.fd >= 0)
        close(backend.fd);
    sensor_stop(&backend.sensor);
    int lcd_cleanup_error = lcd_finish(backend.lcd);
    if (lcd_cleanup_error)
    {
        fprintf(stderr, "WARN LCD shutdown: %s; motor fd already closed\n", strerror(lcd_cleanup_error));
        result = EXIT_FAILURE;
    }
    /* Block our handlers before closing their write fd to avoid fd reuse races. */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, NULL);
    wake_write_fd = -1;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return result;
invalid_options:
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
}
