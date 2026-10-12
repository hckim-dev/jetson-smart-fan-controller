/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "controller.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MONITOR "build/test-bmp180-process"
static struct sensor_sample next_sample(struct sensor_reader *reader)
{
    uint64_t deadline = fan_now_ms() + 4000;
    struct sensor_sample sample;
    while (fan_now_ms() < deadline)
    {
        int result = sensor_next(reader, &sample, fan_now_ms());
        assert(result >= 0);
        if (result)
            return sample;
        struct pollfd descriptor = {.fd = reader->fd, .events = POLLIN};
        assert(poll(&descriptor, 1, 100) >= 0 || errno == EINTR);
    }
    assert(!"sensor frame timeout");
    return (struct sensor_sample){0};
}
static void wait_reaped(struct sensor_reader *reader)
{
    uint64_t deadline = fan_now_ms() + 3000;
    while (reader->pid && fan_now_ms() < deadline)
    {
        sensor_reap(reader);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(reader->pid == 0 && reader->exited);
}
static unsigned int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned int count = 0;
    while (readdir(directory))
        ++count;
    closedir(directory);
    return count;
}
static void assert_reaped(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
}
static void test_delivery_and_stop(void)
{
    unsigned int before = fd_count();
    for (unsigned int i = 0; i < 5; ++i)
    {
        struct sensor_reader reader;
        assert(sensor_start(&reader, MONITOR, "steady") == 0);
        pid_t pid = reader.pid;
        assert(pid > 0 && pid != getpid());
        assert(fcntl(reader.fd, F_GETFL) & O_NONBLOCK);
        assert(fcntl(reader.fd, F_GETFD) & FD_CLOEXEC);
        struct sensor_sample first = next_sample(&reader);
        struct sensor_sample second = next_sample(&reader);
        assert(first.sequence == 1 && second.sequence == 2);
        assert(first.temperature_mc == 27000 && second.pressure_pa == 101325);
        assert(second.timestamp_ms >= first.timestamp_ms);
        sensor_stop(&reader);
        assert(reader.fd == -1 && reader.pid == 0 && reader.exited);
        assert(WIFEXITED(reader.wait_status) && WEXITSTATUS(reader.wait_status) == 0);
        assert_reaped(pid);
        sensor_stop(&reader);
        assert(fd_count() == before);
    }
    puts("PASS process: production fork/exec + valid IPC + SIGTERM/waitpid; repeated fd cleanup");
}
static void test_exec_descriptor_isolation(void)
{
    int guard = open("/dev/null", O_RDONLY | O_CLOEXEC);
    assert(guard >= 0);
    char text[32];
    snprintf(text, sizeof(text), "%d", guard);
    assert(setenv("SMARTFAN_TEST_GUARD_FD", text, 1) == 0);
    struct sensor_reader reader;
    assert(sensor_start(&reader, MONITOR, "steady") == 0);
    struct sensor_sample sample = next_sample(&reader);
    assert(!sample.error);              /* fixture가 자신의 fd를 열기 전에 guard fd 격리를 검사한다. */
    assert(fcntl(guard, F_GETFD) >= 0); /* 부모는 fd를 계속 소유한다. */
    sensor_stop(&reader);
    close(guard);
    assert(unsetenv("SMARTFAN_TEST_GUARD_FD") == 0);
    puts("PASS process: CLOEXEC isolates controller descriptors from exec child");
}
static void test_failure_and_control(void)
{
    struct controller controller = {.sensor = {.fd = -1}, .sensor_enabled = true, .auto_mode = true, .auto_armed = true, .auto_rearm_pending = true};
    fan_device_init(&controller.device);
    controller.device.dry_run = true;
    assert(sensor_start(&controller.sensor, MONITOR, "fault-after-first") == 0);
    controller.sample = next_sample(&controller.sensor);
    controller.have_sample = true;
    assert(controller_auto_tick(&controller) == 0);
    struct smartfan_status state;
    assert(fan_device_get(&controller.device, &state) == 0 && state.running);
    controller.sample = next_sample(&controller.sensor);
    assert(controller.sample.error == EIO && controller.sample.sequence == 2);
    assert(controller_auto_tick(&controller) == 0);
    assert(!controller.auto_armed);
    assert(fan_device_get(&controller.device, &state) == 0 && !state.running);
    wait_reaped(&controller.sensor);
    assert(WIFEXITED(controller.sensor.wait_status) && WEXITSTATUS(controller.sensor.wait_status) == 1);
    sensor_stop(&controller.sensor);
    /* fixture 센서만 복구해도 AUTO가 다시 arm되면 안 된다. */
    assert(sensor_start(&controller.sensor, MONITOR, "steady") == 0);
    controller.sample = next_sample(&controller.sensor);
    assert(controller_auto_tick(&controller) == 0);
    assert(fan_device_get(&controller.device, &state) == 0 && !state.running);
    volatile sig_atomic_t stopped = 0;
    bool known_running = false;
    char on[] = "on";
    assert(controller_command(&controller, on, &known_running, false, false, &stopped) == 0);
    assert(known_running);
    assert(fan_device_set(&controller.device, false) == 0);
    sensor_stop(&controller.sensor);
    puts("PASS process: IPC sensor error disarms AUTO; fresh recovery needs explicit ON");
}
static void test_stop_with_full_pipe(void)
{
    struct sensor_reader reader;
    assert(sensor_start(&reader, MONITOR, "fast") == 0);
    int capacity = fcntl(reader.fd, F_GETPIPE_SZ), queued = 0;
    assert(capacity > 0);
    uint64_t deadline = fan_now_ms() + 3000;
    while (queued < capacity && fan_now_ms() < deadline)
    {
        assert(ioctl(reader.fd, FIONREAD, &queued) == 0);
        const struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(queued == capacity);
    /* 부모가 읽지 않아 write가 막혀도 SIGTERM으로 정상 종료해야 한다. */
    sensor_stop(&reader);
    assert(reader.fd == -1 && reader.pid == 0 && reader.exited);
    assert(WIFEXITED(reader.wait_status) && WEXITSTATUS(reader.wait_status) == 0);
    puts("PASS process: full pipe blocked writer terminates cleanly without SIGPIPE");
}

static void test_forced_stop(void)
{
    struct sensor_reader reader;
    assert(sensor_start(&reader, MONITOR, "ignore-term") == 0);
    pid_t pid = reader.pid;
    next_sample(&reader); /* exec 뒤 SIGTERM 무시 설정을 완료한 상태. */
    sensor_stop(&reader);
    assert(reader.pid == 0 && reader.exited);
    assert(WIFSIGNALED(reader.wait_status) && WTERMSIG(reader.wait_status) == SIGKILL);
    assert_reaped(pid);
    puts("PASS process: ignored SIGTERM escalates to SIGKILL and is reaped");
}
static void test_parent_death(void)
{
    /* 직접 부모가 강제 종료된 monitor를 subreaper가 받아 회수를 검증한다. */
    assert(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    int report[2];
    assert(pipe2(report, O_CLOEXEC) == 0);
    pid_t owner = getpid();
    pid_t parent = fork();
    assert(parent >= 0);
    if (!parent)
    {
        close(report[0]);
        assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() == owner);
        struct sensor_reader reader;
        assert(sensor_start(&reader, MONITOR, "steady") == 0);
        next_sample(&reader); /* parent-death 신호와 PID 검사를 완료한 상태. */
        assert(write(report[1], &reader.pid, sizeof(reader.pid)) == sizeof(reader.pid));
        for (;;)
            pause(); /* sensor_stop 없이 부모를 강제 종료한다. */
    }
    close(report[1]);
    struct pollfd descriptor = {.fd = report[0], .events = POLLIN};
    assert(poll(&descriptor, 1, 5000) == 1);
    pid_t child;
    assert(read(report[0], &child, sizeof(child)) == sizeof(child));
    assert(kill(parent, SIGKILL) == 0);
    close(report[0]);
    int status;
    assert(waitpid(parent, &status, 0) == parent && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    uint64_t deadline = fan_now_ms() + 3000;
    pid_t result = 0;
    while (!result && fan_now_ms() < deadline)
    {
        result = waitpid(child, &status, WNOHANG);
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    assert(result == child);
    /* 갑작스러운 부모 종료는 SIGPIPE와 경합할 수 있으나 잔류 자식은 없어야 한다. */
    assert((WIFEXITED(status) && WEXITSTATUS(status) == 0) ||
           (WIFSIGNALED(status) && (WTERMSIG(status) == SIGTERM || WTERMSIG(status) == SIGPIPE)));
    assert(prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);
    puts("PASS process: production monitor exits after parent death; no orphan remains");
}
int main(void)
{
    test_delivery_and_stop();
    test_exec_descriptor_isolation();
    test_failure_and_control();
    test_stop_with_full_pipe();
    test_forced_stop();
    test_parent_death();
    return 0;
}
