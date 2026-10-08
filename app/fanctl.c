/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "smartfan_uapi.h"

struct backend
{
    int fd;
    bool dry_run;
    struct smartfan_status state;
    uint64_t lease_deadline;
    uint64_t on_deadline;
};

static volatile sig_atomic_t received_signal;
static int wake_write_fd = -1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void handle_signal(int signo)
{
    int saved_errno = errno;
    unsigned char byte = (unsigned char)signo;
    received_signal = signo;
    if (wake_write_fd >= 0)
    {
        ssize_t result = write(wake_write_fd, &byte, sizeof(byte));
        (void)result;
    }
    errno = saved_errno;
}

static const char *reason_name(uint32_t reason)
{
    switch (reason)
    {
    case SMARTFAN_STOP_INITIAL:
        return "initial";
    case SMARTFAN_STOP_USER:
        return "user";
    case SMARTFAN_STOP_CLOSE:
        return "close";
    case SMARTFAN_STOP_LEASE:
        return "lease";
    case SMARTFAN_STOP_MAX_ON:
        return "max_on";
    case SMARTFAN_STOP_REMOVE:
        return "remove";
    default:
        return "unknown";
    }
}

static void dry_expire(struct backend *backend, uint64_t now)
{
    if (!backend->state.running)
        return;
    if (now >= backend->on_deadline)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_MAX_ON;
    }
    else if (now >= backend->lease_deadline)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_LEASE;
    }
}

static int backend_get(struct backend *backend, struct smartfan_status *state)
{
    if (!backend->dry_run)
    {
        memset(state, 0, sizeof(*state));
        if (ioctl(backend->fd, SMARTFAN_IOC_GET, state) < 0)
            return -1;
        if (state->abi_version != SMARTFAN_ABI_VERSION)
        {
            errno = EPROTO;
            return -1;
        }
        return 0;
    }
    uint64_t now = now_ms();
    dry_expire(backend, now);
    *state = backend->state;
    state->lease_remaining_ms = state->running ? (uint32_t)(backend->lease_deadline - now) : 0;
    state->on_remaining_ms = state->running ? (uint32_t)(backend->on_deadline - now) : 0;
    return 0;
}

static int backend_set(struct backend *backend, bool enabled)
{
    struct smartfan_request request = {
        .abi_version = SMARTFAN_ABI_VERSION,
        .enabled = enabled ? 1U : 0U,
        .reserved = {0, 0},
    };
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_SET, &request);
    uint64_t now = now_ms();
    dry_expire(backend, now);
    if (!enabled)
    {
        backend->state.running = 0;
        backend->state.stop_reason = SMARTFAN_STOP_USER;
    }
    else
    {
        if (!backend->state.running)
            backend->on_deadline = now + backend->state.max_on_ms;
        backend->state.running = 1;
        backend->lease_deadline = now + backend->state.lease_ms;
    }
    return 0;
}

static int backend_heartbeat(struct backend *backend)
{
    if (!backend->dry_run)
        return ioctl(backend->fd, SMARTFAN_IOC_HEARTBEAT, 0UL);
    uint64_t now = now_ms();
    dry_expire(backend, now);
    if (!backend->state.running)
    {
        errno = backend->state.stop_reason == SMARTFAN_STOP_LEASE ||
                        backend->state.stop_reason == SMARTFAN_STOP_MAX_ON
                    ? ETIMEDOUT
                    : EPIPE;
        return -1;
    }
    backend->lease_deadline = now + backend->state.lease_ms;
    return 0;
}

static void print_state(const struct smartfan_status *state)
{
    if (state->running)
    {
        printf("STATE ON lease_remaining_ms=%" PRIu32
               " on_remaining_ms=%" PRIu32 "\n",
               (uint32_t)state->lease_remaining_ms,
               (uint32_t)state->on_remaining_ms);
    }
    else
    {
        printf("STATE OFF reason=%s\n", reason_name(state->stop_reason));
    }
}

static void usage(FILE *out, const char *program)
{
    fprintf(out, "Usage: %s [--dry-run | --device PATH]\n"
                 "       [--lease-ms N --max-on-ms N] (dry-run only)\n"
                 "Commands: on, off, status, heartbeat, help, quit\n"
                 "Keep this foreground process open while the fan is ON.\n",
            program);
}

static int parse_ms(const char *text, uint32_t minimum, uint32_t maximum,
                    uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (!text[0] || text[0] < '0' || text[0] > '9')
        return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed < minimum || parsed > maximum)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

/* Return 1 for quit, -1 for a backend error, 0 to continue. */
static int command(struct backend *backend, char *line, bool *known_running)
{
    struct smartfan_status state;
    if (received_signal)
        return 1;
    while (*line == ' ' || *line == '\t')
        ++line;
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\r' ||
                      line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
    if (!length)
        return 0;
    if (!strcmp(line, "quit"))
        return 1;
    if (!strcmp(line, "help"))
    {
        puts("Commands: on, off, status, heartbeat, help, quit");
        return 0;
    }
    if (!strcmp(line, "on"))
    {
        if (received_signal)
            return 1;
        if (backend_set(backend, true) < 0)
            return -1;
    }
    else if (!strcmp(line, "off"))
    {
        if (backend_set(backend, false) < 0)
            return -1;
    }
    else if (!strcmp(line, "heartbeat"))
    {
        if (backend_heartbeat(backend) < 0 &&
            errno != EPIPE && errno != ETIMEDOUT)
            return -1;
    }
    else if (strcmp(line, "status"))
    {
        fprintf(stderr, "ERR unknown command: %s\n", line);
        return 0;
    }
    if (backend_get(backend, &state) < 0)
        return -1;
    *known_running = state.running != 0;
    print_state(&state);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"dry-run", no_argument, NULL, 'n'},
        {"device", required_argument, NULL, 'd'},
        {"lease-ms", required_argument, NULL, 'l'},
        {"max-on-ms", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct backend backend = {
        .fd = -1,
        .state = {
            .abi_version = SMARTFAN_ABI_VERSION,
            .lease_ms = SMARTFAN_DEFAULT_LEASE_MS,
            .max_on_ms = SMARTFAN_DEFAULT_MAX_ON_MS,
            .stop_reason = SMARTFAN_STOP_INITIAL,
        },
    };
    struct smartfan_status state;
    struct sigaction action = {0};
    const char *device = "/dev/smartfan";
    const char *exit_reason = "eof";
    bool device_selected = false, timeout_selected = false;
    bool known_running = false, done = false, discarding = false;
    int wake_pipe[2], option, result = EXIT_SUCCESS;
    uint64_t next_heartbeat = 0;
    uint32_t heartbeat_interval;
    char line[64];
    size_t used = 0;

    _Static_assert(sizeof(struct smartfan_request) == 16, "SET ABI size");
    _Static_assert(sizeof(struct smartfan_status) == 32, "GET ABI size");
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1)
    {
        switch (option)
        {
        case 'n':
            backend.dry_run = true;
            break;
        case 'd':
            device = optarg;
            device_selected = true;
            break;
        case 'l':
            timeout_selected = true;
            if (parse_ms(optarg, 500U, 10000U, &backend.state.lease_ms))
                goto invalid_options;
            break;
        case 'm':
            timeout_selected = true;
            if (parse_ms(optarg, 1000U, 120000U, &backend.state.max_on_ms))
                goto invalid_options;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            goto invalid_options;
        }
    }
    if (optind != argc || (backend.dry_run && device_selected) ||
        (!backend.dry_run && timeout_selected) ||
        backend.state.lease_ms > backend.state.max_on_ms)
        goto invalid_options;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
    {
        perror("pipe2");
        return EXIT_FAILURE;
    }
    wake_write_fd = wake_pipe[1];
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0)
    {
        perror("sigaction");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (backend.dry_run)
    {
        puts("DRY-RUN: hardware access disabled");
    }
    else
    {
        backend.fd = open(device, O_RDWR | O_CLOEXEC);
        if (backend.fd < 0)
        {
            perror(device);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("DEVICE %s\n", device);
    }
    if (backend_get(&backend, &state) < 0)
    {
        perror("get status");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    heartbeat_interval = state.lease_ms / 4U;
    if (heartbeat_interval > 250U)
        heartbeat_interval = 250U;
    if (heartbeat_interval < 50U)
        heartbeat_interval = 50U;
    printf("READY lease_ms=%" PRIu32 " max_on_ms=%" PRIu32 "\n",
           (uint32_t)state.lease_ms, (uint32_t)state.max_on_ms);
    print_state(&state);
    known_running = state.running != 0;

    while (!done && !received_signal)
    {
        struct pollfd descriptors[2] = {
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = wake_pipe[0], .events = POLLIN},
        };
        uint64_t now = now_ms();
        if (backend_get(&backend, &state) < 0)
        {
            perror("get status");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (known_running && !state.running)
            print_state(&state);
        known_running = state.running != 0;
        if (state.running && now >= next_heartbeat)
        {
            if (backend_heartbeat(&backend) < 0)
            {
                if (errno != EPIPE && errno != ETIMEDOUT)
                {
                    perror("heartbeat");
                    result = EXIT_FAILURE;
                    exit_reason = "error";
                    break;
                }
            }
            next_heartbeat = now_ms() + heartbeat_interval;
        }
        int ready = poll(descriptors, 2, 50);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (received_signal || descriptors[1].revents)
            break;
        if (descriptors[0].revents & (POLLERR | POLLNVAL))
        {
            fprintf(stderr, "ERR stdin unavailable\n");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP))
        {
            char bytes[128];
            ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                perror("read stdin");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            if (!count)
                break;
            for (ssize_t i = 0; i < count && !done && !received_signal; ++i)
            {
                if (bytes[i] == '\n')
                {
                    if (discarding)
                    {
                        fprintf(stderr, "ERR command too long or contains NUL\n");
                    }
                    else
                    {
                        line[used] = '\0';
                        int response = command(&backend, line, &known_running);
                        if (response == 1)
                        {
                            done = true;
                            exit_reason = "quit";
                        }
                        else if (response < 0)
                        {
                            perror("command");
                            done = true;
                            result = EXIT_FAILURE;
                            exit_reason = "error";
                        }
                    }
                    used = 0;
                    discarding = false;
                }
                else if (!discarding)
                {
                    if (bytes[i] == '\0' || used == sizeof(line) - 1U)
                        discarding = true;
                    else
                        line[used++] = bytes[i];
                }
            }
        }
    }
    if (received_signal)
        exit_reason = "signal";
    if (backend_set(&backend, false) < 0)
    {
        perror("request OFF");
        result = EXIT_FAILURE;
    }
    else
    {
        printf("CLOSED state=OFF reason=%s\n", exit_reason);
    }
cleanup:
    if (backend.fd >= 0)
        close(backend.fd);
    /* Block our handlers before closing their write fd to avoid fd reuse races. */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    sigprocmask(SIG_BLOCK, &block, NULL);
    wake_write_fd = -1;
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return result;
invalid_options:
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
}
