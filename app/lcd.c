/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "lcd.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct lcd_display
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    char bus[128];
    unsigned int address;
    bool dry_run, stopping, pending, running;
    unsigned int level, leds;
    bool automatic, sensor_enabled, temperature_valid;
    int32_t temperature_mc;
    int error;
};

void lcd_encode_byte(uint8_t value, bool data, uint8_t output[6])
{
    uint8_t flags = 0x08U | (data ? 0x01U : 0U);
    uint8_t high = (value & 0xf0U) | flags;
    uint8_t low = ((value << 4U) & 0xf0U) | flags;
    const uint8_t encoded[6] = {high, high | 0x04U, high,
                                low, low | 0x04U, low};
    memcpy(output, encoded, sizeof(encoded));
}

void lcd_format(bool running, unsigned int level, unsigned int leds,
                char rows[2][17])
{
    lcd_format_environment(running, level, leds, false, false, false, 0, rows);
}

void lcd_format_environment(bool running, unsigned int level, unsigned int leds,
                            bool automatic, bool sensor_enabled, bool temperature_valid,
                            int32_t temperature_mc, char rows[2][17])
{
    char text[64];
    for (unsigned int row = 0; row < 2; ++row)
    {
        if (!row)
            snprintf(text, sizeof(text), "FAN %s %s", running ? "ON" : "OFF",
                     automatic ? "AUTO" : "MANUAL");
        else if (sensor_enabled)
        {
            if (temperature_valid)
            {
                int64_t value = temperature_mc;
                bool negative = value < 0;
                if (negative)
                    value = -value;
                snprintf(text, sizeof(text), "T:%s%lld.%lldC S:%u/5", negative ? "-" : "",
                         (long long)(value / 1000), (long long)((value / 100) % 10), level);
            }
            else
                snprintf(text, sizeof(text), "T:ERR S:%u/5", level);
        }
        else
            snprintf(text, sizeof(text), "SPEED:%u/5 LED:%u", level, leds);
        memset(rows[row], ' ', 16);
        size_t length = strlen(text);
        memcpy(rows[row], text, length < 16 ? length : 16);
        rows[row][16] = '\0';
    }
}

static void delay_us(long usecs)
{
    struct timespec delay = {.tv_sec = usecs / 1000000L,
                             .tv_nsec = (usecs % 1000000L) * 1000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
    {
    }
}

static int port_write(int fd, const uint8_t *bytes, size_t length)
{
    ssize_t count = write(fd, bytes, length);
    if (count < 0)
        return -1;
    if ((size_t)count != length)
    {
        errno = EIO;
        return -1;
    }
    /* A partial/failed I2C transaction may already have strobed E. Do not
     * retry individual bytes and silently desynchronize the 4-bit protocol.
     */
    return 0;
}

static int byte_write(int fd, uint8_t value, bool data)
{
    uint8_t bytes[6];
    lcd_encode_byte(value, data, bytes);
    if (port_write(fd, bytes, sizeof(bytes)) < 0)
        return -1;
    delay_us(!data && (value == 0x01U || value == 0x02U) ? 2000 : 60);
    return 0;
}

static int initialize(int fd)
{
    const uint8_t initial = 0x08U; /* RW/E low, backlight on. */
    if (port_write(fd, &initial, 1) < 0)
        return -1;
    delay_us(50000);
    /* HD44780 instruction initialization when power-on reset is unreliable. */
    for (unsigned int i = 0; i < 4; ++i)
    {
        uint8_t nibble = (i == 3 ? 0x20U : 0x30U) | 0x08U;
        uint8_t bytes[3] = {nibble, nibble | 0x04U, nibble};
        if (port_write(fd, bytes, sizeof(bytes)) < 0)
            return -1;
        delay_us(i == 0 ? 5000 : 200);
    }
    const uint8_t commands[] = {0x28, 0x08, 0x01, 0x06, 0x0c};
    for (size_t i = 0; i < sizeof(commands); ++i)
        if (byte_write(fd, commands[i], false) < 0)
            return -1;
    return 0;
}

static int render(int fd, char rows[2][17], char previous[2][17], bool *valid)
{
    for (unsigned int row = 0; row < 2; ++row)
    {
        if (*valid && !memcmp(rows[row], previous[row], 16))
            continue;
        if (byte_write(fd, row ? 0xc0U : 0x80U, false) < 0)
            return -1;
        for (unsigned int column = 0; column < 16; ++column)
            if (byte_write(fd, (uint8_t)rows[row][column], true) < 0)
                return -1;
    }
    memcpy(previous, rows, 34);
    *valid = true;
    return 0;
}

static void *worker(void *argument)
{
    struct lcd_display *display = argument;
    int fd = -1, failure = 0;
    char previous[2][17] = {{0}};
    bool valid = false;
    if (!display->dry_run)
    {
        if (!strcmp(display->bus, "/dev/i2c-7"))
        {
            unsigned char clock[4] = {0};
            FILE *file = fopen("/proc/device-tree/bus@0/i2c@c250000/clock-frequency", "rb");
            if (!file)
            {
                failure = errno;
                goto finish;
            }
            size_t size = fread(clock, 1, sizeof(clock), file);
            fclose(file);
            unsigned int hz = ((unsigned int)clock[0] << 24) |
                              ((unsigned int)clock[1] << 16) | ((unsigned int)clock[2] << 8) | clock[3];
            if (size != 4 || hz != 100000)
            {
                failure = EOPNOTSUPP;
                goto finish;
            }
        }
        fd = open(display->bus, O_RDWR | O_CLOEXEC);
        if (fd < 0)
        {
            failure = errno;
            goto finish;
        }
        /* Never use I2C_SLAVE_FORCE: respect existing kernel consumers. */
        if (ioctl(fd, I2C_SLAVE, display->address) < 0 || initialize(fd) < 0)
        {
            failure = errno;
            goto finish;
        }
    }
    for (;;)
    {
        char rows[2][17];
        pthread_mutex_lock(&display->lock);
        while (!display->pending && !display->stopping)
            pthread_cond_wait(&display->changed, &display->lock);
        if (!display->pending && display->stopping)
        {
            pthread_mutex_unlock(&display->lock);
            break;
        }
        lcd_format_environment(display->running, display->level, display->leds,
                               display->automatic, display->sensor_enabled,
                               display->temperature_valid, display->temperature_mc, rows);
        display->pending = false;
        pthread_mutex_unlock(&display->lock);
        if (display->dry_run)
        {
            if (!valid || memcmp(rows, previous, sizeof(rows)))
                printf("LCD row1=\"%s\" row2=\"%s\"\n", rows[0], rows[1]);
            memcpy(previous, rows, sizeof(rows));
            valid = true;
        }
        else if (render(fd, rows, previous, &valid) < 0)
        {
            failure = errno;
            break;
        }
    }
finish:
    if (fd >= 0)
        close(fd);
    pthread_mutex_lock(&display->lock);
    display->error = failure;
    pthread_mutex_unlock(&display->lock);
    return NULL;
}

int lcd_start(struct lcd_display **result, const char *bus, unsigned int address,
              bool dry_run)
{
    if (!result || !bus || !bus[0] || strlen(bus) >= sizeof(((struct lcd_display *)0)->bus) ||
        !((address >= 0x20 && address <= 0x27) || (address >= 0x38 && address <= 0x3f)))
    {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    struct lcd_display *display = calloc(1, sizeof(*display));
    if (!display)
        return -1;
    strcpy(display->bus, bus);
    display->address = address;
    display->dry_run = dry_run;
    int ret = pthread_mutex_init(&display->lock, NULL);
    if (ret)
    {
        free(display);
        errno = ret;
        return -1;
    }
    ret = pthread_cond_init(&display->changed, NULL);
    if (ret)
    {
        pthread_mutex_destroy(&display->lock);
        free(display);
        errno = ret;
        return -1;
    }
    /* Main owns the process control-signal flag/self-pipe. The worker must
     * not inherit unblocked control signals, including after a join timeout.
     */
    sigset_t controls, previous_mask;
    sigemptyset(&controls);
    sigaddset(&controls, SIGINT);
    sigaddset(&controls, SIGTERM);
    sigaddset(&controls, SIGHUP);
    ret = pthread_sigmask(SIG_BLOCK, &controls, &previous_mask);
    if (ret)
    {
        pthread_cond_destroy(&display->changed);
        pthread_mutex_destroy(&display->lock);
        free(display);
        errno = ret;
        return -1;
    }
    ret = pthread_create(&display->thread, NULL, worker, display);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    if (ret)
    {
        pthread_cond_destroy(&display->changed);
        pthread_mutex_destroy(&display->lock);
        free(display);
        errno = ret;
        return -1;
    }
    *result = display;
    return 0;
}

void lcd_publish(struct lcd_display *display, bool running, unsigned int level,
                 unsigned int leds)
{
    lcd_publish_environment(display, running, level, leds, false, false, false, 0);
}

void lcd_publish_environment(struct lcd_display *display, bool running, unsigned int level,
                             unsigned int leds, bool automatic, bool sensor_enabled,
                             bool temperature_valid, int32_t temperature_mc)
{
    if (!display)
        return;
    pthread_mutex_lock(&display->lock);
    if (!display->stopping && !display->error &&
        (display->running != running || display->level != level || display->leds != leds ||
         display->automatic != automatic || display->sensor_enabled != sensor_enabled ||
         display->temperature_valid != temperature_valid || display->temperature_mc != temperature_mc))
    {
        display->running = running;
        display->level = level;
        display->leds = leds;
        display->automatic = automatic;
        display->sensor_enabled = sensor_enabled;
        display->temperature_valid = temperature_valid;
        display->temperature_mc = temperature_mc;
        display->pending = true;
        pthread_cond_signal(&display->changed);
    }
    pthread_mutex_unlock(&display->lock);
}

int lcd_error(struct lcd_display *display)
{
    if (!display)
        return 0;
    pthread_mutex_lock(&display->lock);
    int error = display->error;
    pthread_mutex_unlock(&display->lock);
    return error;
}

int lcd_finish(struct lcd_display *display)
{
    if (!display)
        return 0;
    pthread_mutex_lock(&display->lock);
    display->running = false;
    display->leds = 0;
    display->pending = true;
    display->stopping = true;
    pthread_cond_signal(&display->changed);
    pthread_mutex_unlock(&display->lock);
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
        return errno;
    deadline.tv_sec += 2;
    int result = pthread_clockjoin_np(display->thread, NULL, CLOCK_MONOTONIC, &deadline);
    if (result)
        return result;
    pthread_cond_destroy(&display->changed);
    pthread_mutex_destroy(&display->lock);
    free(display);
    return 0;
}
