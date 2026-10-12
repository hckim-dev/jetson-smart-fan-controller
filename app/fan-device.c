/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include "fan-device.h"

void fan_device_init(struct fan_device *device)
{
    *device = (struct fan_device){
        .fd = -1,
        .level = SMARTFAN_MAX_SPEED_LEVEL,
        .last_nonzero = SMARTFAN_MAX_SPEED_LEVEL,
        .auto_inhibited = true,
        .state = {.abi_version = SMARTFAN_ABI_VERSION,
                  .lease_ms = SMARTFAN_DEFAULT_LEASE_MS,
                  .max_on_ms = SMARTFAN_DEFAULT_MAX_ON_MS,
                  .stop_reason = SMARTFAN_STOP_INITIAL},
    };
}

uint64_t fan_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) < 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void dry_expire(struct fan_device *device, uint64_t now)
{
    if (!device->state.running)
        return;
    if (now >= device->on_deadline)
    {
        device->state.running = 0;
        device->state.stop_reason = SMARTFAN_STOP_MAX_ON;
    }
    else if (now >= device->lease_deadline)
    {
        device->state.running = 0;
        device->state.stop_reason = SMARTFAN_STOP_LEASE;
    }
    if (!device->state.running)
    {
        device->led_test = false;
        device->auto_inhibited = true;
    }
}

int fan_device_get(struct fan_device *device, struct smartfan_status *state)
{
    if (!device->dry_run)
    {
        memset(state, 0, sizeof(*state));
        if (ioctl(device->fd, SMARTFAN_IOC_GET, state) < 0)
            return -1;
        if (state->abi_version != SMARTFAN_ABI_VERSION)
        {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    uint64_t now = fan_now_ms();
    dry_expire(device, now);
    *state = device->state;
    state->lease_remaining_ms = state->running ? (uint32_t)(device->lease_deadline - now) : 0;
    state->on_remaining_ms = state->running ? (uint32_t)(device->on_deadline - now) : 0;
    return 0;
}

int fan_device_set(struct fan_device *device, bool enabled)
{
    struct smartfan_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .enabled = enabled ? 1U : 0U,
        .reserved = {0, 0},
    };
    if (!device->dry_run)
        return ioctl(device->fd, SMARTFAN_IOC_SET, &request);
    uint64_t now = fan_now_ms();
    dry_expire(device, now);
    device->led_test = false;
    if (!enabled)
    {
        device->state.running = 0;
        device->state.stop_reason = SMARTFAN_STOP_USER;
    }
    else
    {
        if (!device->state.running)
        {
            if (!device->level)
                device->level = device->last_nonzero;
            device->on_deadline = now + device->state.max_on_ms;
            device->boost_deadline = device->level < SMARTFAN_MAX_SPEED_LEVEL
                                         ? now + SMARTFAN_DEFAULT_BOOST_MS
                                         : 0;
        }
        device->state.running = 1;
        device->auto_inhibited = false;
        device->lease_deadline = now + device->state.lease_ms;
    }
    return 0;
}

int fan_device_heartbeat(struct fan_device *device)
{
    if (!device->dry_run)
        return ioctl(device->fd, SMARTFAN_IOC_HEARTBEAT, 0UL);
    uint64_t now = fan_now_ms();
    dry_expire(device, now);
    if (!device->state.running)
    {
        errno = device->state.stop_reason == SMARTFAN_STOP_LEASE ||
                        device->state.stop_reason == SMARTFAN_STOP_MAX_ON
                    ? ETIMEDOUT
                    : EPIPE;
        return -1;
    }
    device->lease_deadline = now + device->state.lease_ms;
    return 0;
}

int fan_device_get_speed(struct fan_device *device, struct smartfan_speed_status *speed)
{
    memset(speed, 0, sizeof(*speed));
    if (!device->dry_run && !device->legacy)
    {
        if (ioctl(device->fd, SMARTFAN_IOC_GET_SPEED, speed) == 0)
        {
            if (speed->abi_version != SMARTFAN_ABI_VERSION ||
                speed->max_level != SMARTFAN_MAX_SPEED_LEVEL ||
                speed->level > speed->max_level || speed->duty_percent > 100)
            {
                errno = EPROTO;
                return -1;
            }
            device->level = speed->level;
            return 0;
        }
        if (errno != ENOTTY)
            return -1;
        device->legacy = true;
    }
    struct smartfan_status state;
    if (fan_device_get(device, &state) < 0)
        return -1;
    uint64_t now = fan_now_ms();
    speed->abi_version = SMARTFAN_ABI_VERSION;
    speed->level = device->level;
    speed->max_level = SMARTFAN_MAX_SPEED_LEVEL;
    if (device->dry_run)
    {
        speed->capabilities = SMARTFAN_CAP_PWM | SMARTFAN_CAP_LED_BAR | SMARTFAN_CAP_AUTO;
        speed->period_ns = SMARTFAN_DEFAULT_PWM_PERIOD_NS;
        if (state.running && now < device->boost_deadline)
            speed->boost_remaining_ms = (uint32_t)(device->boost_deadline - now);
    }
    speed->duty_percent = !state.running ? 0 : speed->boost_remaining_ms ? 100
                                                                         : smartfan_level_duty(device->level);
    return 0;
}

int fan_device_set_speed(struct fan_device *device, uint32_t level)
{
    struct smartfan_speed_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .level = level,
    };
    if (!device->dry_run)
    {
        if (device->legacy)
        {
            errno = EOPNOTSUPP;
            return -1;
        }
        return ioctl(device->fd, SMARTFAN_IOC_SET_SPEED, &request);
    }
    dry_expire(device, fan_now_ms());
    device->led_test = false;
    device->level = level;
    if (!level)
        return fan_device_set(device, false);
    device->last_nonzero = level;
    if (device->state.running)
        device->lease_deadline = fan_now_ms() + device->state.lease_ms;
    return 0;
}

int fan_device_get_leds(struct fan_device *device, struct smartfan_led_status *leds)
{
    memset(leds, 0, sizeof(*leds));
    if (!device->dry_run)
    {
        if (ioctl(device->fd, SMARTFAN_IOC_GET_LEDS, leds) < 0)
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
    dry_expire(device, fan_now_ms());
    leds->abi_version = SMARTFAN_ABI_VERSION;
    leds->available = 1;
    leds->mode = device->led_test ? SMARTFAN_LED_TEST : SMARTFAN_LED_AUTO;
    leds->count = device->led_test ? device->led_count : device->state.running ? smartfan_level_leds(device->level)
                                                                               : 0;
    return 0;
}

int fan_device_set_leds(struct fan_device *device, bool test, uint32_t count)
{
    struct smartfan_led_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .mode = test ? SMARTFAN_LED_TEST : SMARTFAN_LED_AUTO,
        .count = count,
    };
    if (!device->dry_run)
        return ioctl(device->fd, SMARTFAN_IOC_SET_LEDS, &request);
    dry_expire(device, fan_now_ms());
    if (test && device->state.running)
    {
        errno = EBUSY;
        return -1;
    }
    device->led_test = test;
    device->led_count = count;
    return 0;
}

int fan_device_auto_update(struct fan_device *device, uint32_t level, bool rearm, uint64_t sample_boottime_ms)
{
    struct smartfan_auto_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .level = level,
        .rearm = rearm,
        .sample_boottime_ms = sample_boottime_ms,
    };
    if (!device->dry_run)
        return ioctl(device->fd, SMARTFAN_IOC_SET_AUTO, &request);
    uint64_t now = fan_now_ms();
    dry_expire(device, now);
    if (!smartfan_auto_sample_fresh(now, request.sample_boottime_ms))
    {
        fan_device_set(device, false);
        device->auto_inhibited = true;
        errno = ESTALE;
        return -1;
    }
    if (!smartfan_auto_rearm_allowed(device->auto_inhibited, request.rearm))
    {
        errno = ECANCELED;
        return -1;
    }
    device->auto_inhibited = false;
    if (fan_device_set_speed(device, level) < 0)
        return -1;
    return level ? fan_device_set(device, true) : 0;
}
