/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "controller.h"
#include "encoder.h"

bool controller_sensor_fresh(const struct controller *controller)
{
    uint64_t now = fan_now_ms();
    return controller->have_sample && !controller->sample.error &&
           now >= controller->sample.timestamp_ms &&
           now - controller->sample.timestamp_ms < SENSOR_MAX_AGE_MS;
}

static enum sensor_health sensor_state(const struct controller *controller)
{
    if (!controller->have_sample)
        return SENSOR_PENDING;
    if (controller->sample.error)
        return SENSOR_ERROR;
    return controller_sensor_fresh(controller) ? SENSOR_VALID : SENSOR_STALE;
}

static const char *reason_name(uint32_t reason);

static void observe_sensor_health(struct controller *controller)
{
    if (!controller->sensor_enabled)
        return;
    enum sensor_health state = sensor_state(controller);
    enum sensor_health previous = controller->stats.previous_health;
    bool failed = state == SENSOR_ERROR || state == SENSOR_STALE;
    bool was_failed = previous == SENSOR_ERROR || previous == SENSOR_STALE;
    if (failed && !was_failed)
        ++controller->stats.sensor_faults;
    else if (state == SENSOR_VALID && was_failed)
        ++controller->stats.sensor_recoveries;
    controller->stats.previous_health = state;
}

void controller_print_stats(struct controller *controller)
{
    observe_sensor_health(controller);
    struct event_log_stats log;
    event_log_snapshot(controller->log, &log);
    uint64_t now = fan_now_ms();
    uint64_t uptime = now >= controller->stats.started_ms ? now - controller->stats.started_ms : 0;
    printf("STATS uptime_ms=%" PRIu64 " commands=%" PRIu64
           " sensor_frames=%" PRIu64 " injected_samples=%" PRIu64
           " sensor_faults=%" PRIu64 " sensor_recoveries=%" PRIu64
           " encoder_cw=%" PRIu64 " encoder_ccw=%" PRIu64 "\n",
           uptime, controller->stats.commands, controller->stats.sensor_frames,
           controller->stats.injected_samples, controller->stats.sensor_faults,
           controller->stats.sensor_recoveries, controller->stats.encoder_cw, controller->stats.encoder_ccw);
    printf("LOG_STATS enabled=%u written=%" PRIu64 " dropped=%" PRIu64
           " queued=%u error=%d\n",
           controller->log != NULL, log.written,
           log.dropped, log.queued, log.error);
}

int controller_monitor_tick(struct controller *controller)
{
    if (!controller->monitor_interval_ms)
        return 0;
    uint64_t now = fan_now_ms();
    if (now < controller->next_monitor_ms)
        return 0;
    struct smartfan_status state;
    struct smartfan_speed_status speed;
    if (fan_device_get_speed(&controller->device, &speed) < 0 ||
        fan_device_get(&controller->device, &state) < 0)
        return -1;
    struct event_log_stats log;
    event_log_snapshot(controller->log, &log);
    const char *health = "disabled";
    if (controller->sensor_enabled)
    {
        const char *names[] = {"pending", "valid", "error", "stale"};
        health = names[sensor_state(controller)];
    }
    printf("MONITOR mode=%s armed=%u running=%u level=%u duty=%u%% sensor=%s"
           " temp_mc=%" PRId32 " pressure_pa=%" PRId32 " sensor_pid=%ld"
           " stop_reason=%s lease_remaining_ms=%u on_remaining_ms=%u"
           " log_dropped=%" PRIu64 " log_error=%d\n",
           controller->auto_mode ? "AUTO" : "MANUAL", controller->auto_armed,
           (unsigned int)state.running, (unsigned int)speed.level,
           state.running ? (unsigned int)speed.duty_percent : 0U,
           health, controller->sample.temperature_mc, controller->sample.pressure_pa,
           (long)controller->sensor.pid, state.running ? "none" : reason_name(state.stop_reason),
           (unsigned int)state.lease_remaining_ms, (unsigned int)state.on_remaining_ms,
           log.dropped, log.error);
    /* 지연이나 suspend 후 지난 주기만큼 몰아서 출력하지 않는다. */
    controller->next_monitor_ms = now + controller->monitor_interval_ms;
    return 0;
}

static void print_environment(struct controller *controller)
{
    printf("MODE %s armed=%u target=%u\n", controller->auto_mode ? "AUTO" : "MANUAL",
           controller->auto_armed, controller->auto_target);
    if (!controller->sensor_enabled)
        return;
    controller->reported_sensor_state = sensor_state(controller);
    controller->reported_sensor_error = controller->sample.error;
    if (!controller->have_sample)
        puts("SENSOR pending");
    else if (controller->sample.error)
        printf("SENSOR error=%s\n", strerror(controller->sample.error));
    else if (!controller_sensor_fresh(controller))
        puts("SENSOR stale");
    else
        printf("SENSOR temperature=%.1fC pressure=%.2fhPa age_ms=%" PRIu64 "\n",
               controller->sample.temperature_mc / 1000.0, controller->sample.pressure_pa / 100.0,
               fan_now_ms() - controller->sample.timestamp_ms);
}

/* 정상 sample은 제어/LCD만 갱신한다. 장애·복구·stale 전이는 한 번 알리고 status는 항상 값을 표시한다. */

bool controller_report_sensor_transition(struct controller *controller)
{
    if (!controller->sensor_enabled)
        return false;
    enum sensor_health state = sensor_state(controller);
    if (state == controller->reported_sensor_state &&
        (state != SENSOR_ERROR || controller->sample.error == controller->reported_sensor_error))
        return false;
    event_log_submit(controller->log, "SENSOR health=%d error=%d", (int)state, controller->sample.error);
    print_environment(controller);
    return true;
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

int controller_print_speed(struct controller *controller)
{
    struct smartfan_speed_status speed;
    if (fan_device_get_speed(&controller->device, &speed) < 0)
        return -1;
    printf("SPEED level=%" PRIu32 "/%" PRIu32 " duty=%" PRIu32
           "%% pwm=%u period_ns=%" PRIu32 " boost_remaining_ms=%" PRIu32 "\n",
           (uint32_t)speed.level, (uint32_t)speed.max_level,
           (uint32_t)speed.duty_percent, !!(speed.capabilities & SMARTFAN_CAP_PWM),
           (uint32_t)speed.period_ns, (uint32_t)speed.boost_remaining_ms);
    struct smartfan_led_status leds;
    if (fan_device_get_leds(&controller->device, &leds) < 0)
        return -1;
    printf("LEDBAR available=%u mode=%s count=%u/8\n", (unsigned int)leds.available,
           leds.mode == SMARTFAN_LED_TEST ? "test" : "auto", (unsigned int)leds.count);
    event_log_submit(controller->log, "STATE mode=%s armed=%u level=%u duty=%u leds=%u sensor_valid=%u temp_mc=%" PRId32 " pressure_pa=%" PRId32,
                     controller->auto_mode ? "AUTO" : "MANUAL", controller->auto_armed,
                     (unsigned int)speed.level, (unsigned int)speed.duty_percent, (unsigned int)leds.count,
                     controller->sensor_enabled && controller_sensor_fresh(controller),
                     controller->sample.temperature_mc, controller->sample.pressure_pa);
    print_environment(controller);
    if (controller->lcd)
    {
        struct smartfan_status state;
        if (fan_device_get(&controller->device, &state) < 0)
            return -1;
        lcd_publish_environment(controller->lcd, state.running != 0, speed.level,
                                !state.running && leds.mode == SMARTFAN_LED_AUTO ? 0 : leds.count,
                                controller->auto_mode, controller->sensor_enabled, controller_sensor_fresh(controller),
                                controller->sample.temperature_mc);
    }
    return 0;
}

/* 커널은 출력 변경과 같은 mutex 안에서 AUTO 차단 상태와 sample freshness를 검증한다. */
int controller_auto_tick(struct controller *controller)
{
    observe_sensor_health(controller);
    if (!controller->auto_mode)
        return 0;
    if (!controller_sensor_fresh(controller))
    {
        if (controller->auto_armed)
        {
            controller->auto_armed = false;
            controller->auto_rearm_pending = false;
            if (fan_device_set(&controller->device, false) < 0)
                return -1;
            event_log_submit(controller->log, "AUTO_STOP reason=sensor_fault_or_stale");
            puts("AUTO STOP reason=sensor_fault_or_stale; explicit ON required");
            return controller_print_speed(controller);
        }
        return 0;
    }
    struct smartfan_status state;
    struct smartfan_speed_status speed;
    if (fan_device_get(&controller->device, &state) < 0 || fan_device_get_speed(&controller->device, &speed) < 0)
        return -1;
    if (controller->auto_armed && !controller->auto_rearm_pending && !state.running &&
        (state.stop_reason == SMARTFAN_STOP_MAX_ON || state.stop_reason == SMARTFAN_STOP_LEASE ||
         state.stop_reason == SMARTFAN_STOP_PWM_ERROR || state.stop_reason == SMARTFAN_STOP_SUSPEND ||
         state.stop_reason == SMARTFAN_STOP_SENSOR))
    {
        controller->auto_armed = false;
        event_log_submit(controller->log, "AUTO_STOP reason=driver_limit");
        puts("AUTO STOP reason=driver_limit; explicit ON required");
        return 0;
    }
    controller->auto_target = auto_temperature_level(controller->auto_target, controller->sample.temperature_mc);
    bool changed = speed.level != controller->auto_target;
    if (controller->auto_armed && (changed || (controller->auto_target && !state.running) || controller->auto_rearm_pending))
    {
        bool rearm = controller->auto_rearm_pending;
        controller->auto_rearm_pending = false;
        if (fan_device_auto_update(&controller->device, controller->auto_target, rearm, controller->sample.timestamp_ms) < 0)
        {
            if (errno == ECANCELED || errno == ESTALE || errno == EHOSTDOWN)
            {
                controller->auto_armed = false;
                event_log_submit(controller->log, "AUTO_STOP reason=atomic_guard");
                puts("AUTO STOP reason=atomic_guard; explicit ON required");
                return 0;
            }
            return -1;
        }
        if (controller->auto_target && !state.running)
        {
            puts("AUTO RUN: fresh temperature requested a nonzero level");
            changed = true;
        }
    }
    else if (!controller->auto_armed && changed && fan_device_set_speed(&controller->device, controller->auto_target) < 0)
        return -1;
    return changed ? controller_print_speed(controller) : 0;
}

int controller_change_level(struct controller *controller, int delta)
{
    struct smartfan_speed_status speed;
    if (fan_device_get_speed(&controller->device, &speed) < 0)
        return -1;
    return fan_device_set_speed(&controller->device, encoder_clamp_level(speed.level, delta, speed.max_level));
}

void controller_print_state(const struct smartfan_status *state)
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

int controller_parse_ms(const char *text, uint32_t minimum, uint32_t maximum,
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

/* 반환값: 1은 종료, -1은 장치 오류, 0은 계속 실행. */

int controller_command(struct controller *controller, char *line, bool *known_running,
                       bool encoder_enabled, bool reverse_encoder, const volatile sig_atomic_t *stop_signal)
{
    struct smartfan_status state;
    if (*stop_signal)
        return 1;
    while (*line == ' ' || *line == '\t')
        ++line;
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\r' ||
                      line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
    if (!length)
        return 0;
    ++controller->stats.commands;
    event_log_submit(controller->log, "CMD %s", line);
    printf("CMD %s\n", line);
    if (!strcmp(line, "quit"))
        return 1;
    if (!strcmp(line, "help"))
    {
        puts("Commands: on, off, status, speed 0..5, up, down, led 0..8, led auto, mode manual, mode auto, heartbeat, stats, monitor [500..10000|off], help, quit\nDry-run sensor: temp N, temp error");
        return 0;
    }
    if (!strcmp(line, "stats"))
    {
        controller_print_stats(controller);
        return 0;
    }
    if (!strcmp(line, "monitor") || !strncmp(line, "monitor ", 8))
    {
        if (!strcmp(line, "monitor off"))
        {
            controller->monitor_interval_ms = 0;
            puts("MONITOR disabled");
            return 0;
        }
        uint32_t interval = 1000;
        if (strcmp(line, "monitor") && controller_parse_ms(line + 8, 500, 10000, &interval))
        {
            fputs("ERR monitor interval must be 500..10000 ms or off\n", stderr);
            return 0;
        }
        controller->monitor_interval_ms = interval;
        controller->next_monitor_ms = 0;
        return controller_monitor_tick(controller);
    }
    if (!strcmp(line, "mode auto") || !strcmp(line, "mode manual"))
    {
        bool automatic = !strcmp(line, "mode auto");
        struct smartfan_speed_status speed;
        if (automatic && (!controller->sensor_enabled ||
                          fan_device_get_speed(&controller->device, &speed) < 0 || !(speed.capabilities & SMARTFAN_CAP_AUTO)))
        {
            fputs("ERR AUTO requires --bmp180 and an updated atomic-AUTO driver\n", stderr);
            return 0;
        }
        if (fan_device_set(&controller->device, false) < 0)
            return -1;
        controller->auto_mode = automatic;
        controller->auto_armed = false;
        controller->auto_rearm_pending = false;
        controller->auto_target = 0;
    }
    else if (!strncmp(line, "temp ", 5))
    {
        if (!controller->device.dry_run || !controller->sensor_enabled)
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
        controller->sample = (struct sensor_sample){.magic = SENSOR_MAGIC, .sequence = 1, .error = fault ? EIO : 0, .temperature_mc = temperature, .pressure_pa = 101325, .timestamp_ms = fan_now_ms()};
        controller->have_sample = true;
        ++controller->stats.injected_samples;
    }
    else if (controller->auto_mode && (!strncmp(line, "speed ", 6) ||
                                       !strcmp(line, "up") || !strcmp(line, "down") ||
                                       (!strncmp(line, "led ", 4) && strcmp(line, "led auto")) ||
                                       !strcmp(line, "cw") || !strcmp(line, "ccw")))
    {
        fputs("ERR manual speed/LED controls require mode manual\n", stderr);
        return 0;
    }
    else if (!strcmp(line, "on"))
    {
        if (*stop_signal)
            return 1;
        if (controller->auto_mode)
        {
            if (!controller_sensor_fresh(controller))
            {
                fputs("ERR AUTO ON requires a fresh valid sensor sample\n", stderr);
                return 0;
            }
            controller->auto_armed = true;
            controller->auto_rearm_pending = true;
        }
        else if (fan_device_set(&controller->device, true) < 0)
            return -1;
    }
    else if (!strcmp(line, "off"))
    {
        controller->auto_armed = false;
        controller->auto_rearm_pending = false;
        if (fan_device_set(&controller->device, false) < 0)
            return -1;
    }
    else if (!strcmp(line, "heartbeat"))
    {
        if (fan_device_heartbeat(&controller->device) < 0 &&
            errno != EPIPE && errno != ETIMEDOUT)
            return -1;
    }
    else if (!strncmp(line, "speed ", 6))
    {
        uint32_t level;
        if (controller_parse_ms(line + 6, 0, SMARTFAN_MAX_SPEED_LEVEL, &level))
        {
            fputs("ERR speed must be an integer 0..5\n", stderr);
            return 0;
        }
        if (fan_device_set_speed(&controller->device, level) < 0)
            return -1;
    }
    else if (!strncmp(line, "led ", 4))
    {
        uint32_t count = 0;
        bool test = strcmp(line + 4, "auto") != 0;
        if (test && controller_parse_ms(line + 4, 0, SMARTFAN_LED_SEGMENTS, &count))
        {
            fputs("ERR led must be an integer 0..8 or auto\n", stderr);
            return 0;
        }
        if (fan_device_set_leds(&controller->device, test, count) < 0)
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
        if (controller_change_level(controller, !strcmp(line, "up") ? 1 : -1) < 0)
            return -1;
    }
    else if (controller->device.dry_run && encoder_enabled &&
             (!strcmp(line, "cw") || !strcmp(line, "ccw")))
    {
        int delta = !strcmp(line, "cw") ? 1 : -1;
        delta = reverse_encoder ? -delta : delta;
        if (delta > 0)
            ++controller->stats.encoder_cw;
        else
            ++controller->stats.encoder_ccw;
        if (controller_change_level(controller, delta) < 0)
            return -1;
    }
    else if (strcmp(line, "status"))
    {
        fprintf(stderr, "ERR unknown command: %s\n", line);
        return 0;
    }
    if (controller_auto_tick(controller) < 0 || fan_device_get(&controller->device, &state) < 0)
        return -1;
    event_log_submit(controller->log, "RESULT running=%u stop_reason=%u",
                     (unsigned int)state.running, (unsigned int)state.stop_reason);
    *known_running = state.running != 0;
    controller_print_state(&state);
    return controller_print_speed(controller);
}
