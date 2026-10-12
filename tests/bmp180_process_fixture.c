/* SPDX-License-Identifier: GPL-2.0 */
/* Link the production monitor main against sensor stubs: no I2C/GPIO. */
#define _GNU_SOURCE
#include "bmp180.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fault;
static unsigned int measurements;
int bmp180_open(struct bmp180_device *device, const char *bus)
{
    const char *guard = getenv("SMARTFAN_TEST_GUARD_FD");
    if (guard && (fcntl(atoi(guard), F_GETFD) >= 0 || errno != EBADF))
    {
        errno = EBUSY; /* An inherited controller descriptor survived exec. */
        return -1;
    }
    fault = !strcmp(bus, "fault-after-first");
    if (!strcmp(bus, "ignore-term"))
    {
        struct sigaction action = {.sa_handler = SIG_IGN};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) < 0)
            return -1;
    }
    device->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    return device->fd < 0 ? -1 : 0;
}
int bmp180_measure(struct bmp180_device *device, int32_t *temperature, int32_t *pressure)
{
    (void)device;
    if (fault && measurements++)
    {
        errno = EIO;
        return -1;
    }
    *temperature = 27000;
    *pressure = 101325;
    return 0;
}
void bmp180_close(struct bmp180_device *device)
{
    if (device->fd >= 0)
        close(device->fd);
    device->fd = -1;
}
