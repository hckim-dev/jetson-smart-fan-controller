/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "sensor.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    int pipefd[2];
    assert(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    struct sensor_reader reader = {.fd = pipefd[0]};
    struct sensor_sample source = {.magic = SENSOR_MAGIC, .sequence = 1, .temperature_mc = 27000, .pressure_pa = 101325, .timestamp_ms = 1000};
    struct sensor_sample received;
    assert(write(pipefd[1], &source, 7) == 7);
    assert(sensor_next(&reader, &received, 1000) == 0);
    assert(write(pipefd[1], (const char *)&source + 7, sizeof(source) - 7) == sizeof(source) - 7);
    assert(sensor_next(&reader, &received, 1000) == 1 && received.temperature_mc == 27000);
    assert(sensor_next(&reader, &received, 1000) == 0);
    source.sequence = 3;
    assert(write(pipefd[1], &source, sizeof(source)) == sizeof(source));
    assert(sensor_next(&reader, &received, 1000) == -1 && errno == EPROTO);
    source.sequence = 2;
    source.timestamp_ms = 1101;
    assert(write(pipefd[1], &source, sizeof(source)) == sizeof(source));
    assert(sensor_next(&reader, &received, 1000) == -1 && errno == EPROTO);
    source.timestamp_ms = 1000;
    source.pressure_pa = 0;
    assert(write(pipefd[1], &source, sizeof(source)) == sizeof(source));
    assert(sensor_next(&reader, &received, 1000) == -1 && errno == EPROTO);
    source.error = EIO;
    assert(write(pipefd[1], &source, sizeof(source)) == sizeof(source));
    assert(sensor_next(&reader, &received, 1000) == 1 && received.error == EIO);
    close(pipefd[1]);
    assert(sensor_next(&reader, &received, 1000) == -1 && errno == EPIPE);
    sensor_stop(&reader);

    /* EOF in the middle of a frame must be a protocol error, not clean EOF. */
    assert(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    reader = (struct sensor_reader){.fd = pipefd[0]};
    assert(write(pipefd[1], &source, 7) == 7);
    close(pipefd[1]);
    assert(sensor_next(&reader, &received, 1000) == 0);
    assert(sensor_next(&reader, &received, 1000) == -1 && errno == EPROTO);
    sensor_stop(&reader);

    /* Exercise the real fork/exec/IPC/error/reap path without touching I2C. */
    assert(sensor_start(&reader, "build/bmp180-monitor", "/dev/null/not-an-i2c-bus") == 0);
    const struct timespec interval = {.tv_nsec = 1000000L};
    int result = 0;
    for (unsigned int i = 0; i < 2000 && !result; ++i)
    {
        result = sensor_next(&reader, &received, UINT64_C(1000000000000));
        if (!result)
            nanosleep(&interval, NULL);
    }
    assert(result == 1 && received.error == ENOTDIR);
    for (unsigned int i = 0; i < 1000 && reader.pid; ++i)
    {
        sensor_reap(&reader);
        if (reader.pid)
            nanosleep(&interval, NULL);
    }
    assert(reader.pid == 0);
    sensor_stop(&reader);
    assert(reader.fd == -1 && reader.pid == 0);
    sensor_stop(&reader);
    assert(sensor_start(&reader, "/dev/null/not-a-program", "/dev/i2c-7") == 0);
    result = 0;
    for (unsigned int i = 0; i < 2000 && !result; ++i)
    {
        result = sensor_next(&reader, &received, UINT64_C(1000000000000));
        if (!result)
            nanosleep(&interval, NULL);
    }
    assert(result == 1 && received.error == ENOTDIR);
    sensor_stop(&reader);
    puts("PASS sensor IPC: partial reads, EAGAIN, sequence errors, EOF, fork/exec error frame, cleanup");
    return 0;
}
