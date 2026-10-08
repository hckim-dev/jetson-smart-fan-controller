/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef SMARTFAN_UAPI_H
#define SMARTFAN_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SMARTFAN_ABI_VERSION 1U
#define SMARTFAN_DEFAULT_LEASE_MS 2000U
#define SMARTFAN_DEFAULT_MAX_ON_MS 30000U

enum smartfan_stop_reason
{
	SMARTFAN_STOP_INITIAL = 0,
	SMARTFAN_STOP_USER = 1,
	SMARTFAN_STOP_CLOSE = 2,
	SMARTFAN_STOP_LEASE = 3,
	SMARTFAN_STOP_MAX_ON = 4,
	SMARTFAN_STOP_REMOVE = 5,
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

#endif
