/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "sensor.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

unsigned int auto_temperature_level(unsigned int previous, int32_t temperature_mc)
{
    const int32_t thresholds[5] = {24000, 26000, 28000, 30000, 32000};
    unsigned int level = previous > 5 ? 5 : previous;
    while (level < 5 && temperature_mc >= thresholds[level])
        ++level;
    while (level && temperature_mc < thresholds[level - 1] - 1000)
        --level;
    return level;
}

int sensor_start(struct sensor_reader *reader, const char *program, const char *bus)
{
    if (!reader || !program || !program[0] || !bus || !bus[0])
    {
        errno = EINVAL;
        return -1;
    }
    int pipefd[2];
    char parent[32];
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
    snprintf(parent, sizeof(parent), "%ld", (long)getpid());
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return -1;
    /* fork~exec 사이에 부모 신호 handler가 자식 SIGTERM을 소비하지 않도록 제어 신호를 차단한다. */
    sigset_t controls, previous_mask;
    sigemptyset(&controls);
    sigaddset(&controls, SIGINT);
    sigaddset(&controls, SIGTERM);
    sigaddset(&controls, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &controls, &previous_mask) < 0)
    {
        int error = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = error;
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        int error = errno;
        sigprocmask(SIG_SETMASK, &previous_mask, NULL);
        close(pipefd[0]);
        close(pipefd[1]);
        errno = error;
        return -1;
    }
    if (!pid)
    {
        /* exec 전에는 async-signal-safe 연산만 사용한다. 제어/I²C fd는 CLOEXEC으로 격리한다. */
        close(pipefd[0]);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) < 0 ||
            sigaction(SIGTERM, &action, NULL) < 0 ||
            sigaction(SIGHUP, &action, NULL) < 0)
            _exit(126);
        sigdelset(&previous_mask, SIGINT);
        sigdelset(&previous_mask, SIGTERM);
        sigdelset(&previous_mask, SIGHUP);
        if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) < 0)
            _exit(126);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (fcntl(STDOUT_FILENO, F_SETFD, 0) < 0)
            _exit(126);
        if (pipefd[1] != STDOUT_FILENO)
            close(pipefd[1]);
        char *const args[] = {(char *)program, "--stream", "--bus", (char *)bus,
                              "--parent-pid", parent, NULL};
        execv(program, args);
        /* exec 실패도 errno frame으로 전달한다. fork 직후 stdio를 사용하거나 EOF만으로 원인을 추측하지 않는다. */
        struct sensor_sample failure = {.magic = SENSOR_MAGIC, .sequence = 1, .error = errno};
        ssize_t written;
        do
        {
            written = write(STDOUT_FILENO, &failure, sizeof(failure));
        } while (written < 0 && errno == EINTR);
        _exit(127);
    }
    close(pipefd[1]);
    reader->fd = pipefd[0];
    reader->pid = pid;
    if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) < 0)
    {
        int error = errno;
        sensor_stop(reader);
        errno = error;
        return -1;
    }
    if (fcntl(reader->fd, F_SETFL, O_NONBLOCK) < 0)
    {
        int error = errno;
        sensor_stop(reader);
        errno = error;
        return -1;
    }
    return 0;
}

int sensor_next(struct sensor_reader *reader, struct sensor_sample *sample, uint64_t now)
{
    ssize_t count = read(reader->fd, reader->buffer + reader->used,
                         sizeof(reader->buffer) - reader->used);
    if (count < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    if (!count)
    {
        errno = reader->used ? EPROTO : EPIPE;
        return -1;
    }
    reader->used += (unsigned int)count;
    if (reader->used != sizeof(reader->buffer))
        return 0;
    memcpy(sample, reader->buffer, sizeof(*sample));
    reader->used = 0;
    if (sample->magic != SENSOR_MAGIC || sample->reserved || sample->error < 0 ||
        sample->error > 4095 || sample->sequence != reader->sequence + 1U ||
        (sample->timestamp_ms > now && sample->timestamp_ms - now > 100) ||
        (!sample->error && (sample->temperature_mc < -40000 || sample->temperature_mc > 85000 ||
                            sample->pressure_pa < 30000 || sample->pressure_pa > 110000)))
    {
        errno = EPROTO;
        return -1;
    }
    reader->sequence = sample->sequence;
    return 1;
}

/* waitpid 회수 또는 ECHILD를 확인할 때까지 PID를 유지한다. 신호를 보냈다는 이유로 소유권을 버리지 않는다. */
static bool collect_child(struct sensor_reader *reader)
{
    int status;
    pid_t result = waitpid(reader->pid, &status, WNOHANG);
    if (result == reader->pid)
    {
        reader->wait_status = status;
        reader->exited = true;
        reader->pid = 0;
        return true;
    }
    if (result < 0 && errno == ECHILD)
    {
        reader->pid = 0;
        return true;
    }
    return false;
}

void sensor_reap(struct sensor_reader *reader)
{
    if (reader->pid > 0)
        collect_child(reader);
}

void sensor_stop(struct sensor_reader *reader)
{
    if (reader->pid <= 0 || collect_child(reader))
        goto close_pipe;
    /* 전송 중인 자식을 SIGPIPE로 종료시키지 않도록 회수까지 read end를 유지한다. */
    kill(reader->pid, SIGTERM);
    const struct timespec delay = {.tv_nsec = 10000000L};
    for (unsigned int i = 0; i < 100; ++i)
    {
        if (collect_child(reader))
            goto close_pipe;
        if (i == 50)
            kill(reader->pid, SIGKILL);
        nanosleep(&delay, NULL);
    }
    fputs("WARN sensor child has not exited; kernel I2C operation may still be pending\n", stderr);
close_pipe:
    if (reader->fd >= 0)
        close(reader->fd);
    reader->fd = -1;
}
