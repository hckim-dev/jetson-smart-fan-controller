/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
static int guarded_ioctl(int fd, unsigned long cmd, ...);
#define ioctl guarded_ioctl
#define main tested_fanctl_main
#include "../app/fanctl.c"
#undef main
#undef ioctl

static bool inhibited, running, inject_stale;
static uint32_t level;
static unsigned int auto_calls, manual_on_calls;
static int guarded_ioctl(int fd, unsigned long cmd, ...)
{
    (void)fd;
    va_list args;
    va_start(args, cmd);
    int result = 0;
    if (cmd == SMARTFAN_IOC_GET)
    {
        struct smartfan_status *status = va_arg(args, struct smartfan_status *);
        *status = (struct smartfan_status){.abi_version = 1, .running = running, .stop_reason = SMARTFAN_STOP_USER};
    }
    else if (cmd == SMARTFAN_IOC_GET_SPEED)
    {
        struct smartfan_speed_status *speed = va_arg(args, struct smartfan_speed_status *);
        *speed = (struct smartfan_speed_status){.abi_version = 1, .level = level, .max_level = 5, .capabilities = SMARTFAN_CAP_PWM | SMARTFAN_CAP_AUTO};
        /* Model a protected kernel stop after the preceding GET snapshot. */
        inhibited = true;
    }
    else if (cmd == SMARTFAN_IOC_SET_AUTO)
    {
        struct smartfan_auto_request *request = va_arg(args, struct smartfan_auto_request *);
        ++auto_calls;
        uint64_t now = now_ms() + (inject_stale ? SMARTFAN_AUTO_MAX_AGE_MS : 0);
        if (!smartfan_auto_sample_fresh(now, request->sample_boottime_ms))
        {
            errno = ESTALE;
            result = -1;
        }
        else if (!smartfan_auto_rearm_allowed(inhibited, request->rearm))
        {
            errno = ECANCELED;
            result = -1;
        }
        else
        {
            inhibited = false;
            level = request->level;
            running = level != 0;
        }
    }
    else if (cmd == SMARTFAN_IOC_SET)
    {
        struct smartfan_request *request = va_arg(args, struct smartfan_request *);
        manual_on_calls += request->enabled;
    }
    else if (cmd == SMARTFAN_IOC_GET_LEDS)
    {
        struct smartfan_led_status *leds = va_arg(args, struct smartfan_led_status *);
        *leds = (struct smartfan_led_status){.abi_version = 1, .available = 1};
    }
    else
    {
        errno = ENOTTY;
        result = -1;
    }
    va_end(args);
    return result;
}

static void scenario(int32_t temp, bool rearm, bool stale, bool expect_running)
{
    level = temp < 24000 ? 1 : 0;
    running = false;
    inhibited = false;
    inject_stale = stale;
    struct backend backend = {.fd = 100, .sensor = {.fd = -1}, .sensor_enabled = true, .have_sample = true, .auto_mode = true, .auto_armed = true, .auto_rearm_pending = rearm, .sample = {.magic = SENSOR_MAGIC, .temperature_mc = temp, .pressure_pa = 101325}};
    backend.sample.timestamp_ms = now_ms();
    assert(auto_tick(&backend) == 0);
    assert(running == expect_running);
    if (!expect_running)
        assert(!backend.auto_armed);
}
int main(void)
{
    _Static_assert(sizeof(struct smartfan_auto_request) == 24, "AUTO ABI layout");
    assert(smartfan_auto_sample_fresh(2999, 0));
    assert(!smartfan_auto_sample_fresh(3000, 0));
    assert(!smartfan_auto_sample_fresh(100, 101));
    scenario(27000, false, false, false); /* Suspend between GET and start. */
    scenario(22000, false, false, false); /* Cold update cannot clear inhibition. */
    scenario(27000, true, true, false);   /* Time passed after frontend check. */
    scenario(27000, true, false, true);   /* Explicit operator ON can rearm. */
    /* A cold, unchanged target must not submit OFF repeatedly at poll rate. */
    level = 0;
    running = false;
    inject_stale = false;
    struct backend cold = {.fd = 100, .sensor = {.fd = -1}, .sensor_enabled = true, .have_sample = true, .auto_mode = true, .auto_armed = true, .sample = {.magic = SENSOR_MAGIC, .temperature_mc = 22000, .pressure_pa = 101325}};
    cold.sample.timestamp_ms = now_ms();
    for (unsigned int i = 0; i < 20; ++i)
        assert(auto_tick(&cold) == 0);
    assert(!running);
    assert(auto_calls == 4 && manual_on_calls == 0);
    puts("PASS atomic AUTO: crossed stop, cold update/idle, expired sample, explicit rearm; no plain ON fallback");
    return 0;
}
