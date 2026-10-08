/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "bmp180.h"
#include "sensor.h"
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop(int signal_number) { stopped = signal_number; }
static uint64_t now_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_BOOTTIME, &value) < 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}
static int emit(const struct sensor_sample *sample, bool stream)
{
    if (!stream)
    {
        if (sample->error)
        {
            fprintf(stderr, "BMP180: %s\n", strerror(sample->error));
            return 0;
        }
        printf("BMP180 temperature=%.1fC pressure=%.2fhPa pressure_pa=%" PRId32 "\n",
               sample->temperature_mc / 1000.0, sample->pressure_pa / 100.0, sample->pressure_pa);
        return 0;
    }
    const unsigned char *bytes = (const unsigned char *)sample;
    size_t sent = 0;
    while (sent < sizeof(*sample))
    {
        ssize_t count = write(STDOUT_FILENO, bytes + sent, sizeof(*sample) - sent);
        if (count < 0 && errno == EINTR && !stopped)
            continue;
        if (count <= 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}
int main(int argc, char **argv)
{
    const char *bus = "/dev/i2c-7";
    bool stream = false, once = false;
    pid_t parent = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--once"))
            once = true;
        else if (!strcmp(argv[i], "--stream"))
            stream = true;
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc)
            bus = argv[++i];
        else if (!strcmp(argv[i], "--parent-pid") && i + 1 < argc)
        {
            char *end;
            errno = 0;
            long value = strtol(argv[++i], &end, 10);
            if (errno || *end || value < 1 || value > INT32_MAX)
                return EXIT_FAILURE;
            parent = (pid_t)value;
        }
        else if (!strcmp(argv[i], "--help"))
        {
            puts("Usage: bmp180-monitor [--once] [--bus /dev/i2c-7]\n"
                 "Internal IPC: --stream --parent-pid PID");
            return EXIT_SUCCESS;
        }
        else
            return EXIT_FAILURE;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        return EXIT_FAILURE;
    if (parent && (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent))
        return EXIT_FAILURE;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct bmp180_device device = {.fd = -1};
    struct sensor_sample sample = {.magic = SENSOR_MAGIC};
    if (bmp180_open(&device, bus) < 0)
    {
        sample.error = errno;
        sample.sequence = 1;
        sample.timestamp_ms = now_ms();
        emit(&sample, stream);
        return EXIT_FAILURE;
    }
    int result = EXIT_SUCCESS;
    while (!stopped)
    {
        sample.error = 0;
        sample.timestamp_ms = now_ms();
        if (bmp180_measure(&device, &sample.temperature_mc, &sample.pressure_pa) < 0)
            sample.error = errno;
        ++sample.sequence;
        if (emit(&sample, stream) < 0 || sample.error)
        {
            result = EXIT_FAILURE;
            break;
        }
        if (once)
            break;
        struct timespec delay = {.tv_sec = 1};
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR && !stopped)
        {
        }
    }
    bmp180_close(&device);
    return result;
}
