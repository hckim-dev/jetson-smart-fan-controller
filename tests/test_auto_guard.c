/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdio.h>
#include "controller.h"

static bool inhibited, running, inject_stale;
static uint32_t level;
static unsigned int auto_calls, manual_on_calls;
int __wrap_ioctl(int fd, unsigned long cmd, ...)
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
        /* GET snapshot 이후 보호 정지가 끼어드는 경쟁 조건을 재현한다. */
        inhibited = true;
    }
    else if (cmd == SMARTFAN_IOC_SET_AUTO)
    {
        struct smartfan_auto_request *request = va_arg(args, struct smartfan_auto_request *);
        ++auto_calls;
        uint64_t now = fan_now_ms() + (inject_stale ? SMARTFAN_AUTO_MAX_AGE_MS : 0);
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
    struct controller backend = {.device = {.fd = 100}, .sensor = {.fd = -1}, .sensor_enabled = true, .have_sample = true, .auto_mode = true, .auto_armed = true, .auto_rearm_pending = rearm, .sample = {.magic = SENSOR_MAGIC, .temperature_mc = temp, .pressure_pa = 101325}};
    backend.sample.timestamp_ms = fan_now_ms();
    assert(controller_auto_tick(&backend) == 0);
    assert(running == expect_running);
    if (!expect_running)
        assert(!backend.auto_armed);
}
static void test_stats_stale_episode(void)
{
    struct controller controller = {.sensor_enabled = true, .have_sample = true, .sample = {.timestamp_ms = fan_now_ms() - SENSOR_MAX_AGE_MS}};
    /* 통계용 health는 터미널 통지/중복 억제 상태와 독립적이다. */
    assert(controller_report_sensor_transition(&controller));
    assert(controller_auto_tick(&controller) == 0);
    assert(controller_auto_tick(&controller) == 0);
    assert(controller.stats.sensor_faults == 1);
    controller.sample.error = EIO;
    assert(controller_auto_tick(&controller) == 0);
    assert(controller.stats.sensor_faults == 1); /* 같은 장애 구간이므로 중복 집계하지 않는다. */
    controller.sample.error = 0;
    controller.sample.timestamp_ms = fan_now_ms();
    assert(controller_auto_tick(&controller) == 0);
    assert(controller.stats.sensor_recoveries == 1);
    controller.sample.error = EIO;
    assert(controller_auto_tick(&controller) == 0);
    assert(controller.stats.sensor_faults == 2);
    puts("PASS stats: stale/error share one episode; recovery and new fault counted independently of status output");
}

int main(void)
{
    test_stats_stale_episode();
    struct controller notices = {.sensor_enabled = true};
    assert(!controller_report_sensor_transition(&notices));
    notices.have_sample = true;
    notices.sample.timestamp_ms = fan_now_ms();
    notices.sample.temperature_mc = 25000;
    assert(controller_report_sensor_transition(&notices));
    for (unsigned int i = 0; i < 100; ++i)
    {
        notices.sample.temperature_mc += 1;
        notices.sample.pressure_pa += 1;
        notices.sample.timestamp_ms = fan_now_ms();
        assert(!controller_report_sensor_transition(&notices));
    }
    notices.sample.error = EIO;
    assert(controller_report_sensor_transition(&notices));
    assert(!controller_report_sensor_transition(&notices));
    notices.sample.error = ENOTDIR;
    assert(controller_report_sensor_transition(&notices));
    notices.sample.error = 0;
    notices.sample.timestamp_ms = fan_now_ms();
    assert(controller_report_sensor_transition(&notices));
    notices.sample.timestamp_ms = fan_now_ms() - SENSOR_MAX_AGE_MS;
    assert(controller_report_sensor_transition(&notices));
    assert(!controller_report_sensor_transition(&notices));
    puts("PASS sensor output: 100 normal samples silent; fault/recovery/stale reported once");
    _Static_assert(sizeof(struct smartfan_auto_request) == 24, "AUTO ABI layout");
    assert(smartfan_auto_sample_fresh(2999, 0));
    assert(!smartfan_auto_sample_fresh(3000, 0));
    assert(!smartfan_auto_sample_fresh(100, 101));
    scenario(27000, false, false, false); /* GET과 기동 사이의 suspend. */
    scenario(22000, false, false, false); /* 저온 갱신도 보호 차단을 해제할 수 없다. */
    scenario(27000, true, true, false);   /* 프런트 검사 이후 sample이 만료됨. */
    scenario(27000, true, false, true);   /* 명시적 ON만 다시 arm할 수 있다. */
    /* 저온의 같은 target은 poll마다 OFF를 반복 전송하지 않는다. */
    level = 0;
    running = false;
    inject_stale = false;
    struct controller cold = {.device = {.fd = 100}, .sensor = {.fd = -1}, .sensor_enabled = true, .have_sample = true, .auto_mode = true, .auto_armed = true, .sample = {.magic = SENSOR_MAGIC, .temperature_mc = 22000, .pressure_pa = 101325}};
    cold.sample.timestamp_ms = fan_now_ms();
    for (unsigned int i = 0; i < 20; ++i)
        assert(controller_auto_tick(&cold) == 0);
    assert(!running);
    assert(auto_calls == 4 && manual_on_calls == 0);
    puts("PASS atomic AUTO: crossed stop, cold update/idle, expired sample, explicit rearm; no plain ON fallback");
    return 0;
}
