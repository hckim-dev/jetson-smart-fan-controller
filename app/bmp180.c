/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "bmp180.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdbool.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8) | p[1]; }
static int16_t signed16(const uint8_t *p)
{
    uint16_t value = be16(p);
    return (int16_t)(value >= 0x8000U ? (int32_t)value - 65536 : value);
}
/* Defined arithmetic right shift, including negative values and INT64_MIN. */
static int64_t sar(int64_t value, unsigned int shift)
{
    int64_t divisor = INT64_C(1) << shift;
    return value >= 0 ? value / divisor : -1 - (-1 - value) / divisor;
}

int bmp180_parse_calibration(const uint8_t bytes[22], struct bmp180_calibration *c)
{
    bool zero = true, erased = true;
    for (unsigned int i = 0; i < 22; ++i)
    {
        zero = zero && bytes[i] == 0;
        erased = erased && bytes[i] == 255;
    }
    *c = (struct bmp180_calibration){
        .ac1 = signed16(bytes),
        .ac2 = signed16(bytes + 2),
        .ac3 = signed16(bytes + 4),
        .ac4 = be16(bytes + 6),
        .ac5 = be16(bytes + 8),
        .ac6 = be16(bytes + 10),
        .b1 = signed16(bytes + 12),
        .b2 = signed16(bytes + 14),
        .mb = signed16(bytes + 16),
        .mc = signed16(bytes + 18),
        .md = signed16(bytes + 20),
    };
    if (zero || erased || !c->ac4 || !c->ac5)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int bmp180_compensate(const struct bmp180_calibration *c, uint32_t ut,
                      uint32_t up, unsigned int oss, int32_t *temperature_mc,
                      int32_t *pressure_pa)
{
    if (oss > 3 || ut > 65535 || up > (UINT32_C(65535) << oss) + ((1U << oss) - 1U) ||
        !c->ac4 || !c->ac5)
    {
        errno = EINVAL;
        return -1;
    }
    int64_t x1 = sar(((int64_t)ut - c->ac6) * c->ac5, 15);
    int64_t denominator = x1 + c->md;
    if (!denominator)
        goto range_error;
    int64_t x2 = (int64_t)c->mc * 2048 / denominator;
    int64_t b5 = x1 + x2;
    int64_t t = sar(b5 + 8, 4);
    /* Validate temperature before squaring B6: malformed coefficients must
     * not turn unbounded intermediate values into signed overflow.
     */
    if (t < -400 || t > 850)
        goto range_error;
    int64_t b6 = b5 - 4000;
    x1 = sar((int64_t)c->b2 * sar(b6 * b6, 12), 11);
    x2 = sar((int64_t)c->ac2 * b6, 11);
    int64_t x3 = x1 + x2;
    int64_t b3 = sar(((int64_t)c->ac1 * 4 + x3) * (INT64_C(1) << oss) + 2, 2);
    x1 = sar((int64_t)c->ac3 * b6, 13);
    x2 = sar((int64_t)c->b1 * sar(b6 * b6, 12), 16);
    x3 = sar(x1 + x2 + 2, 2);
    int64_t b4 = sar((int64_t)c->ac4 * (x3 + 32768), 15);
    int64_t b7 = ((int64_t)up - b3) * (50000 >> oss);
    if (b4 <= 0 || b7 < 0)
        goto range_error;
    int64_t p = b7 < INT64_C(0x80000000) ? b7 * 2 / b4 : (b7 / b4) * 2;
    if (p < 0 || p > 200000)
        goto range_error;
    x1 = sar(p, 8) * sar(p, 8);
    x1 = sar(x1 * 3038, 16);
    x2 = sar(-7357 * p, 16);
    p += sar(x1 + x2 + 3791, 4);
    if (p < 30000 || p > 110000)
        goto range_error;
    *temperature_mc = (int32_t)(t * 100);
    *pressure_pa = (int32_t)p;
    return 0;
range_error:
    errno = ERANGE;
    return -1;
}

static int read_registers(int fd, uint8_t address, uint8_t *bytes, uint16_t length)
{
    struct i2c_msg messages[2] = {
        {.addr = 0x77, .len = 1, .buf = &address},
        {.addr = 0x77, .flags = I2C_M_RD, .len = length, .buf = bytes},
    };
    struct i2c_rdwr_ioctl_data transfer = {.msgs = messages, .nmsgs = 2};
    int result = ioctl(fd, I2C_RDWR, &transfer);
    if (result < 0)
        return -1;
    if (result != 2)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

static void delay_us(long microseconds)
{
    struct timespec delay = {.tv_nsec = microseconds * 1000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
    {
    }
}

static int convert(int fd, uint8_t command, long delay)
{
    uint8_t bytes[2] = {0xf4, command};
    ssize_t count = write(fd, bytes, sizeof(bytes));
    if (count < 0)
        return -1;
    if (count != 2)
    {
        errno = EIO;
        return -1;
    }
    delay_us(delay);
    for (unsigned int i = 0; i < 5; ++i)
    {
        uint8_t control;
        if (read_registers(fd, 0xf4, &control, 1) < 0)
            return -1;
        if (!(control & 0x20))
            return 0;
        delay_us(2000);
    }
    errno = ETIMEDOUT;
    return -1;
}

int bmp180_open(struct bmp180_device *device, const char *bus)
{
    device->fd = open(bus, O_RDWR | O_CLOEXEC);
    if (device->fd < 0)
        return -1;
    unsigned long functions;
    uint8_t id, calibration[22];
    /* Advisory lock only among our sensor readers. LCD does not take this
     * lock; the kernel still serializes separate-address I2C transactions.
     */
    if (flock(device->fd, LOCK_EX | LOCK_NB) < 0 ||
        ioctl(device->fd, I2C_FUNCS, &functions) < 0)
        goto fail;
    if (!(functions & I2C_FUNC_I2C))
    {
        errno = EOPNOTSUPP;
        goto fail;
    }
    if (ioctl(device->fd, I2C_SLAVE, 0x77) < 0 ||
        read_registers(device->fd, 0xd0, &id, 1) < 0)
        goto fail;
    if (id != 0x55)
    {
        errno = ENODEV;
        goto fail;
    }
    if (read_registers(device->fd, 0xaa, calibration, 22) < 0 ||
        bmp180_parse_calibration(calibration, &device->calibration) < 0)
        goto fail;
    return 0;
fail:
    {
        int error = errno;
        bmp180_close(device);
        errno = error;
    }
    return -1;
}

int bmp180_measure(struct bmp180_device *device, int32_t *temperature_mc,
                   int32_t *pressure_pa)
{
    uint8_t temperature[2], pressure[3];
    const unsigned int oss = 3;
    if (convert(device->fd, 0x2e, 5000) < 0 ||
        read_registers(device->fd, 0xf6, temperature, 2) < 0 ||
        convert(device->fd, (uint8_t)(0x34 + (oss << 6)), 26000) < 0 ||
        read_registers(device->fd, 0xf6, pressure, 3) < 0)
        return -1;
    uint32_t up = ((uint32_t)pressure[0] << 16) | ((uint32_t)pressure[1] << 8) | pressure[2];
    return bmp180_compensate(&device->calibration, be16(temperature), up >> (8 - oss),
                             oss, temperature_mc, pressure_pa);
}

void bmp180_close(struct bmp180_device *device)
{
    if (device->fd >= 0)
        close(device->fd);
    device->fd = -1;
}
