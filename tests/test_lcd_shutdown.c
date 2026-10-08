/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int mock_open(const char *path, int flags, ...);
static int mock_ioctl(int fd, unsigned long cmd, ...);
static ssize_t mock_write(int fd, const void *bytes, size_t size);
#define open mock_open
#define ioctl mock_ioctl
#define write mock_write
#include "../app/lcd.c"
#undef open
#undef ioctl
#undef write

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static bool blocked, released;
static int mock_open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    return open("/dev/null", O_RDWR | O_CLOEXEC);
}
static int mock_ioctl(int fd, unsigned long cmd, ...)
{
    (void)fd;
    (void)cmd;
    return 0;
}
static ssize_t mock_write(int fd, const void *bytes, size_t size)
{
    (void)fd;
    (void)bytes;
    sigset_t mask;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGINT) == 1 && sigismember(&mask, SIGTERM) == 1 &&
           sigismember(&mask, SIGHUP) == 1);
    pthread_mutex_lock(&gate);
    blocked = true;
    pthread_cond_signal(&ready);
    while (!released)
        pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    return (ssize_t)size;
}
int main(void)
{
    struct lcd_display *display = NULL;
    sigset_t before, after;
    assert(pthread_sigmask(SIG_SETMASK, NULL, &before) == 0);
    assert(lcd_start(&display, "mock-i2c", 0x27, false) == 0);
    assert(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0);
    assert(sigismember(&before, SIGINT) == sigismember(&after, SIGINT));
    assert(sigismember(&before, SIGTERM) == sigismember(&after, SIGTERM));
    assert(sigismember(&before, SIGHUP) == sigismember(&after, SIGHUP));
    pthread_mutex_lock(&gate);
    while (!blocked)
        pthread_cond_wait(&ready, &gate);
    pthread_mutex_unlock(&gate);
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    assert(lcd_finish(display) == ETIMEDOUT);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    assert(elapsed >= 1.9 && elapsed < 3.5);
    /* Production exits the process on timeout. Test releases the mock and
     * verifies the object was kept alive until the worker actually stopped.
     */
    pthread_mutex_lock(&gate);
    released = true;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&gate);
    assert(pthread_join(display->thread, NULL) == 0);
    pthread_cond_destroy(&display->changed);
    pthread_mutex_destroy(&display->lock);
    free(display);
    puts("PASS LCD shutdown: stuck I2C bounded at 2s, live-worker memory retained, control signal ownership");
    return 0;
}
