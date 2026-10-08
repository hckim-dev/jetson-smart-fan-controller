/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef SMARTFAN_UAPI_H
#define SMARTFAN_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SMARTFAN_ABI_VERSION 1U
#define SMARTFAN_DEFAULT_LEASE_MS 2000U
#define SMARTFAN_DEFAULT_MAX_ON_MS 30000U
#define SMARTFAN_MAX_SPEED_LEVEL 5U
#define SMARTFAN_DEFAULT_PWM_PERIOD_NS 4000000U
#define SMARTFAN_DEFAULT_BOOST_MS 200U
#define SMARTFAN_CAP_PWM 1U
#define SMARTFAN_CAP_LED_BAR 2U
#define SMARTFAN_LED_SEGMENTS 8U

enum smartfan_led_mode {
	SMARTFAN_LED_AUTO = 0,
	SMARTFAN_LED_TEST = 1,
};

enum smartfan_stop_reason
{
	SMARTFAN_STOP_INITIAL = 0,
	SMARTFAN_STOP_USER = 1,
	SMARTFAN_STOP_CLOSE = 2,
	SMARTFAN_STOP_LEASE = 3,
	SMARTFAN_STOP_MAX_ON = 4,
	SMARTFAN_STOP_REMOVE = 5,
	SMARTFAN_STOP_PWM_ERROR = 6,
};

struct smartfan_request
{
	__u32 abi_version;
	__u32 enabled;
	__u32 reserved[2];
};

struct smartfan_status
{
	__u32 abi_version;
	__u32 running;
	__u32 stop_reason;
	__u32 lease_ms;
	__u32 max_on_ms;
	__u32 lease_remaining_ms;
	__u32 on_remaining_ms;
	__u32 reserved;
};

#define SMARTFAN_IOC_SET _IOW('F', 1, struct smartfan_request)
#define SMARTFAN_IOC_HEARTBEAT _IO('F', 2)
#define SMARTFAN_IOC_GET _IOR('F', 3, struct smartfan_status)

/* Additive ABI: the stage-1 SET/GET structures and commands stay unchanged. */
struct smartfan_speed_request {
	__u32 abi_version;
	__u32 level;
	__u32 reserved[2];
};

struct smartfan_speed_status {
	__u32 abi_version;
	__u32 level;
	__u32 max_level;
	__u32 duty_percent;
	__u32 capabilities;
	__u32 period_ns;
	__u32 boost_remaining_ms;
	__u32 reserved;
};

#define SMARTFAN_IOC_SET_SPEED _IOW('F', 4, struct smartfan_speed_request)
#define SMARTFAN_IOC_GET_SPEED _IOR('F', 5, struct smartfan_speed_status)

struct smartfan_led_request {
	__u32 abi_version;
	__u32 mode;
	__u32 count;
	__u32 reserved;
};

struct smartfan_led_status {
	__u32 abi_version;
	__u32 mode;
	__u32 count;
	__u32 available;
};

#define SMARTFAN_IOC_SET_LEDS _IOW('F', 6, struct smartfan_led_request)
#define SMARTFAN_IOC_GET_LEDS _IOR('F', 7, struct smartfan_led_status)

/* Fill from segment 1: levels 0..5 -> 0,2,4,5,7,8 segments. */
static inline __u32 smartfan_level_leds(__u32 level)
{
	return (level * SMARTFAN_LED_SEGMENTS + SMARTFAN_MAX_SPEED_LEVEL - 1U) /
		SMARTFAN_MAX_SPEED_LEVEL;
}

/* Duty targets for levels 0..5; calibration requires a motor test. */
static inline __u32 smartfan_level_duty(__u32 level)
{
	return level ? 50U + level * 10U : 0U;
}

#endif
