/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int __real_open(const char *path, int flags, ...);
static unsigned int expected_period, requests;
static bool expected_edges;

int __wrap_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/mockchip") && flags == (O_RDONLY | O_CLOEXEC));
    return __real_open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    va_start(args, command);
    void *argument = va_arg(args, void *);
    va_end(args);
    if (command == GPIO_GET_CHIPINFO_IOCTL)
    {
        struct gpiochip_info *info = argument;
        strcpy(info->label, "tegra234-gpio");
        info->lines = 200;
    }
    else if (command == GPIO_V2_GET_LINE_IOCTL)
    {
        struct gpio_v2_line_request *request = argument;
        assert(request->num_lines == 2 && request->offsets[0] == 50 && request->offsets[1] == 52);
        assert(request->config.flags == (GPIO_V2_LINE_FLAG_INPUT |
                                         (expected_edges ? GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING : 0)));
        assert(request->config.num_attrs == 1 && request->config.attrs[0].mask == 3);
        assert(request->config.attrs[0].attr.id == GPIO_V2_LINE_ATTR_ID_DEBOUNCE);
        assert(request->config.attrs[0].attr.debounce_period_us == expected_period);
        request->fd = dup(fd);
        assert(request->fd >= 0);
        ++requests;
    }
    else if (command == GPIO_V2_LINE_GET_VALUES_IOCTL)
    {
        struct gpio_v2_line_values *values = argument;
        assert(values->mask == 3);
        values->bits = 3;
    }
    else
    {
        errno = ENOTTY;
        return -1;
    }
    return 0;
}

int main(void)
{
    struct encoder_input input;
    expected_edges = true;
    expected_period = 0;
    assert(encoder_open(&input, "/mockchip", false) == 0);
    assert(input.decoder.state == 3 && input.resync_pending);
    assert(fcntl(input.fd, F_GETFL) & O_NONBLOCK);
    assert(fcntl(input.fd, F_GETFD) & FD_CLOEXEC);
    encoder_close(&input);
    expected_period = 1000;
    assert(encoder_open_with_debounce(&input, "/mockchip", false, 1000) == 0);
    encoder_close(&input);
    expected_edges = false;
    expected_period = 0;
    assert(encoder_open_levels(&input, "/mockchip") == 0);
    encoder_close(&input);
    assert(requests == 3);
    puts("PASS encoder request: default explicit period 0, input/both edges, diagnostics, CLOEXEC/nonblocking; mock GPIO only");
    return 0;
}
