/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "event-log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct event_log
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int fd;
    bool stopping;
    unsigned int head, count;
    uint64_t written, dropped;
    int error;
    char records[EVENT_LOG_CAPACITY][EVENT_LOG_LINE_SIZE];
};
static int write_record(int fd, const char *record)
{
    size_t remaining = strlen(record);
    while (remaining)
    {
        ssize_t count = write(fd, record, remaining);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!count)
            return EIO;
        record += count;
        remaining -= (size_t)count;
    }
    return 0;
}
static void *worker(void *argument)
{
    struct event_log *log = argument;
    for (;;)
    {
        char record[EVENT_LOG_LINE_SIZE];
        pthread_mutex_lock(&log->lock);
        while (!log->count && !log->stopping)
            pthread_cond_wait(&log->changed, &log->lock);
        if (!log->count && log->stopping)
        {
            pthread_mutex_unlock(&log->lock);
            break;
        }
        memcpy(record, log->records[log->head], sizeof(record));
        log->head = (log->head + 1U) % EVENT_LOG_CAPACITY;
        --log->count;
        pthread_mutex_unlock(&log->lock);
        /* 파일 I/O 중에는 queue mutex를 잡지 않는다. */
        int error = write_record(log->fd, record);
        pthread_mutex_lock(&log->lock);
        if (error)
        {
            log->error = error;
            log->dropped += log->count + 1U;
            log->count = 0;
        }
        else
            ++log->written;
        pthread_mutex_unlock(&log->lock);
        if (error)
            break;
    }
    int error = close(log->fd) < 0 ? errno : 0;
    pthread_mutex_lock(&log->lock);
    if (!log->error)
        log->error = error;
    pthread_mutex_unlock(&log->lock);
    return NULL;
}
int event_log_start(struct event_log **result, const char *path)
{
    if (!result || !path || !path[0])
    {
        errno = EINVAL;
        return -1;
    }
    *result = NULL;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0)
        return -1;
    struct stat info;
    int error = 0;
    if (fstat(fd, &info) < 0)
        error = errno;
    else if (!S_ISREG(info.st_mode))
        error = EINVAL;
    if (error)
    {
        close(fd);
        errno = error;
        return -1;
    }
    struct event_log *log = calloc(1, sizeof(*log));
    if (!log)
    {
        error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    log->fd = fd;
    error = pthread_mutex_init(&log->lock, NULL);
    if (error)
        goto free_log;
    error = pthread_cond_init(&log->changed, NULL);
    if (error)
        goto destroy_lock;
    sigset_t controls, previous;
    sigemptyset(&controls);
    sigaddset(&controls, SIGINT);
    sigaddset(&controls, SIGTERM);
    sigaddset(&controls, SIGHUP);
    error = pthread_sigmask(SIG_BLOCK, &controls, &previous);
    if (error)
        goto destroy_condition;
    error = pthread_create(&log->thread, NULL, worker, log);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error)
        goto destroy_condition;
    *result = log;
    return 0;
destroy_condition:
    pthread_cond_destroy(&log->changed);
destroy_lock:
    pthread_mutex_destroy(&log->lock);
free_log:
    close(fd);
    free(log);
    errno = error;
    return -1;
}
int event_log_submit(struct event_log *log, const char *format, ...)
{
    if (!log)
        return 0;
    int saved_errno = errno;
    char record[EVENT_LOG_LINE_SIZE] = {0};
    struct timespec timestamp;
    int error = 0;
    if (!format)
        error = EINVAL;
    else if (clock_gettime(CLOCK_BOOTTIME, &timestamp) < 0)
        error = errno;
    if (!error)
    {
        uint64_t ms = (uint64_t)timestamp.tv_sec * 1000U + (uint64_t)timestamp.tv_nsec / 1000000U;
        int prefix = snprintf(record, sizeof(record), "boottime_ms=%" PRIu64 " ", ms);
        va_list args;
        va_start(args, format);
        int length = vsnprintf(record + prefix, sizeof(record) - (size_t)prefix, format, args);
        va_end(args);
        if (length < 0 || (size_t)length >= sizeof(record) - (size_t)prefix - 1U)
            error = EMSGSIZE;
        else
        {
            for (int i = prefix; i < prefix + length; ++i)
                if (record[i] == '\n' || record[i] == '\r')
                    record[i] = ' ';
            record[prefix + length] = '\n';
            record[prefix + length + 1] = '\0';
        }
    }
    pthread_mutex_lock(&log->lock);
    if (!error)
        error = log->error ? log->error : log->stopping                  ? ESHUTDOWN
                                      : log->count == EVENT_LOG_CAPACITY ? ENOBUFS
                                                                         : 0;
    if (error)
        ++log->dropped;
    else
    {
        unsigned int tail = (log->head + log->count) % EVENT_LOG_CAPACITY;
        memcpy(log->records[tail], record, sizeof(record));
        ++log->count;
        pthread_cond_signal(&log->changed);
    }
    pthread_mutex_unlock(&log->lock);
    errno = saved_errno;
    return error;
}
void event_log_snapshot(struct event_log *log, struct event_log_stats *stats)
{
    if (!stats)
        return;
    *stats = (struct event_log_stats){0};
    if (!log)
        return;
    pthread_mutex_lock(&log->lock);
    *stats = (struct event_log_stats){.written = log->written, .dropped = log->dropped, .queued = log->count, .error = log->error};
    pthread_mutex_unlock(&log->lock);
}
int event_log_finish(struct event_log *log, struct event_log_stats *stats)
{
    if (!log)
    {
        event_log_snapshot(NULL, stats);
        return 0;
    }
    pthread_mutex_lock(&log->lock);
    log->stopping = true;
    pthread_cond_signal(&log->changed);
    pthread_mutex_unlock(&log->lock);
    struct timespec deadline;
    int error;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
        error = errno;
    else
    {
        deadline.tv_sec += 2;
        /* GCC11 TSan이 추적하는 tryjoin을 사용하고 MONOTONIC 기한으로 종료 대기를 제한한다. */
        for (;;)
        {
            error = pthread_tryjoin_np(log->thread, NULL);
            if (error != EBUSY)
                break;
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            {
                error = errno;
                break;
            }
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            {
                error = ETIMEDOUT;
                break;
            }
            const struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
        }
    }
    struct event_log_stats final;
    event_log_snapshot(log, &final);
    if (stats)
        *stats = final;
    if (error)
        return error;
    pthread_cond_destroy(&log->changed);
    pthread_mutex_destroy(&log->lock);
    free(log);
    return final.error;
}
