/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FAN_DEVICE_H
#define FAN_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "smartfan_uapi.h"

/* fd 수명은 main이, ioctl과 dry-run 모델은 이 모듈이 소유한다. GPIO/PWM 자원을 직접 요청하지 않는다. */
struct fan_device
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
    bool auto_inhibited;
};

void fan_device_init(struct fan_device *device);
uint64_t fan_now_ms(void);
int fan_device_get(struct fan_device *device, struct smartfan_status *state);
int fan_device_set(struct fan_device *device, bool enabled);
int fan_device_heartbeat(struct fan_device *device);
int fan_device_get_speed(struct fan_device *device, struct smartfan_speed_status *speed);
int fan_device_set_speed(struct fan_device *device, uint32_t level);
int fan_device_get_leds(struct fan_device *device, struct smartfan_led_status *leds);
int fan_device_set_leds(struct fan_device *device, bool test, uint32_t count);
int fan_device_auto_update(struct fan_device *device, uint32_t level, bool rearm, uint64_t sample_boottime_ms);

#endif
