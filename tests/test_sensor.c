/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "sensor.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int __real_execv(const char *path, char *const argv[]);
pid_t __real_fork(void);
static int fail_fork;

pid_t __wrap_fork(void)
{
    if (fail_fork) { errno = EAGAIN; return -1; }
    return __real_fork();
}

static void test_fork_failure(void)
{
    sigset_t before, after;
    assert(sigprocmask(SIG_SETMASK, NULL, &before) == 0);
    int next_fd = open("/dev/null", O_RDONLY);
    assert(next_fd >= 0);
    close(next_fd);
    struct sensor_reader reader;
    fail_fork = 1;
    assert(sensor_start(&reader, "unused", "unused") == -1 && errno == EAGAIN);
    fail_fork = 0;
    assert(reader.fd == -1 && reader.pid == 0);
    assert(sigprocmask(SIG_SETMASK, NULL, &after) == 0);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP};
    for (unsigned int i = 0; i < 3; ++i)
        assert(sigismember(&before, signals[i]) == sigismember(&after, signals[i]));
    int fd = open("/dev/null", O_RDONLY);
    assert(fd == next_fd); /* Neither pipe end leaked. */
    close(fd);
}

/* Hold the real fork child in the pre-exec window, without GPIO/I2C. */
int __wrap_execv(const char *path, char *const argv[])
{
    if (strcmp(path, "test-pre-exec-window"))
        return __real_execv(path, argv);
    struct sigaction action;
    sigset_t mask;
    if (sigaction(SIGTERM, NULL, &action) < 0 ||
        sigprocmask(SIG_SETMASK, NULL, &mask) < 0) _exit(124);
    struct sensor_sample ready = {.magic = SENSOR_MAGIC, .sequence = 1,
        .error = EINPROGRESS, .temperature_mc = action.sa_handler == SIG_DFL,
        .pressure_pa = sigismember(&mask, SIGTERM) == 0};
    if (write(STDOUT_FILENO, &ready, sizeof(ready)) != sizeof(ready)) _exit(125);
    for (;;) pause();
}

static void parent_signal_handler(int signo) { (void)signo; }

static double monotonic_seconds(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void test_pre_exec_termination(void)
{
    struct sigaction previous, handler = {.sa_handler = parent_signal_handler};
    sigemptyset(&handler.sa_mask);
    assert(sigaction(SIGTERM, &handler, &previous) == 0);
    struct sensor_reader reader;
    struct sensor_sample sample;
    assert(sensor_start(&reader, "test-pre-exec-window", "unused") == 0);
    const struct timespec delay = {.tv_nsec = 1000000};
    int result = 0;
    for (unsigned int i = 0; i < 2000 && !result; ++i) {
        result = sensor_next(&reader, &sample, UINT64_C(1000000000000));
        if (!result) nanosleep(&delay, NULL);
    }
    assert(result == 1 && sample.error == EINPROGRESS);
    assert(sample.temperature_mc == 1 && sample.pressure_pa == 1);
    double start = monotonic_seconds();
    sensor_stop(&reader);
    double elapsed = monotonic_seconds() - start;
    assert(sigaction(SIGTERM, &previous, NULL) == 0);
    printf("pre-exec SIGTERM cleanup: %.3fs\n", elapsed);
    /* Verify disposition/mask directly rather than a scheduler-sensitive
     * wall-clock assertion. The child is ready to terminate on SIGTERM.
     */
    assert(reader.fd == -1 && reader.pid == 0);
}

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
    test_fork_failure();
    test_pre_exec_termination();
    puts("PASS sensor IPC: partial reads, EAGAIN, sequence errors, EOF, fork/exec error frame, pre-exec signal cleanup");
    return 0;
}
