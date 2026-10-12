/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "event-log.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

ssize_t __real_write(int fd, const void *bytes, size_t size);
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool hold, entered, fail_write, interrupted, partial, fail_create;
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    if (fail_create)
        return EAGAIN;
    return __real_pthread_create(thread, attr, start, arg);
}
ssize_t __wrap_write(int fd, const void *bytes, size_t size)
{
    sigset_t mask;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGINT) && sigismember(&mask, SIGTERM) && sigismember(&mask, SIGHUP));
    pthread_mutex_lock(&gate);
    entered = true;
    pthread_cond_broadcast(&changed);
    while (hold)
        pthread_cond_wait(&changed, &gate);
    bool failure = fail_write, retry = interrupted, short_write = partial;
    interrupted = false;
    pthread_mutex_unlock(&gate);
    if (failure)
    {
        errno = ENOSPC;
        return -1;
    }
    if (retry)
    {
        errno = EINTR;
        return -1;
    }
    return __real_write(fd, bytes, short_write && size > 5 ? 5 : size);
}
static void mode(bool blocking, bool failure, bool short_write)
{
    pthread_mutex_lock(&gate);
    hold = blocking;
    fail_write = failure;
    partial = short_write;
    interrupted = short_write;
    entered = false;
    pthread_mutex_unlock(&gate);
}
static void wait_entered(void)
{
    pthread_mutex_lock(&gate);
    while (!entered)
        pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
static void release(void)
{
    pthread_mutex_lock(&gate);
    hold = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void test_queue_and_timeout(const char *path)
{
    mode(true, false, false);
    struct event_log *log;
    sigset_t before, after;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &before) == 0);
    assert(event_log_start(&log, path) == 0);
    assert(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0);
    assert(sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM));
    assert(event_log_submit(log, "first") == 0);
    wait_entered();
    for (unsigned int i = 0; i < EVENT_LOG_CAPACITY; ++i)
    {
        errno = ERANGE;
        assert(event_log_submit(log, "queued %u", i) == 0 && errno == ERANGE);
    }
    assert(event_log_submit(log, "overflow") == ENOBUFS);
    struct event_log_stats stats;
    event_log_snapshot(log, &stats);
    assert(stats.queued == EVENT_LOG_CAPACITY && stats.dropped == 1 && stats.written == 0);
    assert(event_log_finish(log, &stats) == ETIMEDOUT);
    assert(event_log_submit(log, "stopped") == ESHUTDOWN);
    release();
    assert(event_log_finish(log, &stats) == 0);
    assert(stats.written == EVENT_LOG_CAPACITY + 1 && stats.dropped == 2 && !stats.queued);
    puts("PASS log: stalled writer does not hold queue lock; bounded queue, drops, timeout and retry");
}
struct producer
{
    struct event_log *log;
    unsigned int id;
};
static void *produce(void *arg)
{
    struct producer *producer = arg;
    for (unsigned int i = 0; i < 1000; ++i)
    {
        int result = event_log_submit(producer->log, "P %u %u", producer->id, i);
        assert(result == 0 || result == ENOBUFS);
        struct event_log_stats stats;
        event_log_snapshot(producer->log, &stats);
        assert(stats.queued <= EVENT_LOG_CAPACITY && !stats.error);
    }
    return NULL;
}
static void test_concurrent_producers(const char *path)
{
    mode(false, false, false);
    struct event_log *log;
    assert(event_log_start(&log, path) == 0);
    pthread_t threads[4];
    struct producer producers[4];
    for (unsigned int i = 0; i < 4; ++i)
    {
        producers[i] = (struct producer){log, i};
        assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
    }
    for (unsigned int i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    struct event_log_stats stats;
    assert(event_log_finish(log, &stats) == 0);
    assert(stats.written + stats.dropped == 4000 && !stats.queued);
    FILE *file = fopen(path, "r");
    assert(file);
    char line[EVENT_LOG_LINE_SIZE];
    int last[4] = {-1, -1, -1, -1};
    unsigned int lines = 0;
    while (fgets(line, sizeof(line), file))
    {
        unsigned long long ms;
        unsigned int id, sequence;
        assert(sscanf(line, "boottime_ms=%llu P %u %u", &ms, &id, &sequence) == 3);
        assert(id < 4 && sequence < 1000 && (int)sequence > last[id]);
        last[id] = (int)sequence;
        assert(strchr(line, '\n'));
        ++lines;
    }
    fclose(file);
    assert(lines == stats.written);
    puts("PASS log: four producers, 4000 records accounted for, complete ordered records per producer");
}
static void test_errors_and_partial_io(const char *path)
{
    mode(false, false, true);
    struct event_log *log;
    assert(event_log_start(&log, path) == 0);
    assert(event_log_submit(log, "one\ntwo\rthree") == 0);
    char huge[EVENT_LOG_LINE_SIZE + 1];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    assert(event_log_submit(log, "%s", huge) == EMSGSIZE);
    struct event_log_stats stats;
    assert(event_log_finish(log, &stats) == 0 && stats.written == 1 && stats.dropped == 1);
    FILE *file = fopen(path, "r");
    assert(file);
    char line[EVENT_LOG_LINE_SIZE];
    assert(fgets(line, sizeof(line), file) && strstr(line, "one two three\n"));
    assert(!fgets(line, sizeof(line), file));
    fclose(file);
    mode(false, true, false);
    assert(event_log_start(&log, path) == 0);
    assert(event_log_submit(log, "disk failure") == 0);
    assert(event_log_finish(log, &stats) == ENOSPC && stats.error == ENOSPC && stats.dropped == 1);
    fail_create = true;
    assert(event_log_start(&log, path) == -1 && errno == EAGAIN && !log);
    fail_create = false;
    assert(event_log_start(&log, "/dev/null") == -1 && errno == EINVAL);
    assert(event_log_start(&log, "") == -1 && errno == EINVAL);
    assert(event_log_submit(NULL, "disabled") == 0);
    puts("PASS log: partial writes/EINTR, line escaping, oversized record, ENOSPC, create failure and special-file rejection");
}
int main(void)
{
    char directory[] = "/tmp/smartfan-thread-XXXXXX";
    assert(mkdtemp(directory));
    char path[128];
    snprintf(path, sizeof(path), "%s/queue.log", directory);
    test_queue_and_timeout(path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/producers.log", directory);
    test_concurrent_producers(path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/errors.log", directory);
    test_errors_and_partial_io(path);
    unlink(path);
    assert(rmdir(directory) == 0);
    return 0;
}
