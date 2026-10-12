/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "smartfan_uapi.h"
#include "encoder.h"
#include "lcd.h"
#include "sensor.h"

#include "controller.h"

static volatile sig_atomic_t received_signal;
static int wake_write_fd = -1;

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

static void usage(FILE *out, const char *program)
{
    fprintf(out, "Usage: %s [--dry-run | --device PATH]\n"
                 "       [--lease-ms N --max-on-ms N] (dry-run only)\n"
                 "Commands: on, off, status, speed 0..5, up, down, led 0..8, led auto, heartbeat, help, quit\n"
                 "       [--encoder [--encoder-chip PATH] [--reverse-encoder]]\n"
                 "       [--lcd --lcd-address 0x27 [--lcd-bus /dev/i2c-7]]\n"
                 "       [--bmp180 [--sensor-bus /dev/i2c-7]]\n"
                 "Sensor commands: mode manual, mode auto; temp N/error (dry-run only).\n"
                 "Dry-run encoder simulation: cw, ccw (requires --encoder).\n"
                 "       [--log PATH] (append-only asynchronous event log)\n"
                 "Monitor commands: stats, monitor [500..10000|off]\n"
                 "Keep this foreground process open while the fan is ON.\n",
            program);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"dry-run", no_argument, NULL, 'n'},
        {"device", required_argument, NULL, 'd'},
        {"lease-ms", required_argument, NULL, 'l'},
        {"max-on-ms", required_argument, NULL, 'm'},
        {"encoder", no_argument, NULL, 'e'},
        {"encoder-chip", required_argument, NULL, 'c'},
        {"reverse-encoder", no_argument, NULL, 'r'},
        {"lcd", no_argument, NULL, 'L'},
        {"lcd-bus", required_argument, NULL, 'B'},
        {"lcd-address", required_argument, NULL, 'A'},
        {"bmp180", no_argument, NULL, 'S'},
        {"sensor-bus", required_argument, NULL, 'I'},
        {"log", required_argument, NULL, 'g'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct controller controller = {
        .sensor = {.fd = -1},
    };
    fan_device_init(&controller.device);
    controller.stats.started_ms = fan_now_ms();
    struct smartfan_status state;
    struct encoder_input encoder = {.fd = -1};
    bool encoder_enabled = false, reverse_encoder = false;
    bool lcd_enabled = false, lcd_bus_selected = false, lcd_address_selected = false;
    bool lcd_warned = false, log_warned = false;
    const char *log_path = NULL;
    const char *lcd_bus = "/dev/i2c-7";
    unsigned int lcd_address = 0x27;
    const char *sensor_bus = "/dev/i2c-7";
    bool sensor_bus_selected = false;
    bool sensor_exit_reported = false;
    const char *encoder_chip = NULL;
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
    _Static_assert(sizeof(struct smartfan_speed_request) == 16, "SET_SPEED ABI size");
    _Static_assert(sizeof(struct smartfan_speed_status) == 32, "GET_SPEED ABI size");
    _Static_assert(sizeof(struct smartfan_auto_request) == 24, "SET_AUTO ABI size");
    _Static_assert(SENSOR_MAX_AGE_MS == SMARTFAN_AUTO_MAX_AGE_MS, "sensor/kernel age policy");
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1)
    {
        switch (option)
        {
        case 'n':
            controller.device.dry_run = true;
            break;
        case 'd':
            device = optarg;
            device_selected = true;
            break;
        case 'l':
            timeout_selected = true;
            if (controller_parse_ms(optarg, 500U, 10000U, &controller.device.state.lease_ms))
                goto invalid_options;
            break;
        case 'm':
            timeout_selected = true;
            if (controller_parse_ms(optarg, 1000U, 120000U, &controller.device.state.max_on_ms))
                goto invalid_options;
            break;
        case 'e':
            encoder_enabled = true;
            break;
        case 'c':
            encoder_chip = optarg;
            break;
        case 'r':
            reverse_encoder = true;
            break;
        case 'L':
            lcd_enabled = true;
            break;
        case 'S':
            controller.sensor_enabled = true;
            break;
        case 'I':
            sensor_bus = optarg;
            sensor_bus_selected = true;
            break;
        case 'B':
            lcd_bus = optarg;
            lcd_bus_selected = true;
            break;
        case 'A':
        {
            char *end;
            errno = 0;
            if (optarg[0] < '0' || optarg[0] > '9')
                goto invalid_options;
            unsigned long address = strtoul(optarg, &end, 0);
            if (errno || *end || !((address >= 0x20 && address <= 0x27) || (address >= 0x38 && address <= 0x3f)))
                goto invalid_options;
            lcd_address = (unsigned int)address;
            lcd_address_selected = true;
            break;
        }
        case 'g':
            log_path = optarg;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            goto invalid_options;
        }
    }
    if ((log_path && !log_path[0]) || optind != argc || (controller.device.dry_run && device_selected) ||
        (!controller.device.dry_run && timeout_selected) ||
        controller.device.state.lease_ms > controller.device.state.max_on_ms ||
        (!encoder_enabled && (encoder_chip || reverse_encoder)) ||
        (controller.device.dry_run && encoder_chip) ||
        (!lcd_enabled && (lcd_bus_selected || lcd_address_selected)) ||
        (!controller.sensor_enabled && sensor_bus_selected) ||
        (controller.sensor_enabled && (strlen(sensor_bus) >= 128 || !sensor_bus[0])) ||
        (lcd_enabled && (strlen(lcd_bus) >= 128 || !lcd_bus[0] ||
                         (!controller.device.dry_run && !lcd_address_selected))))
        goto invalid_options;

    /* 자원 생성 전에 stdin을 검사한다. 닫힌 fd0이 pipe/device로 재사용되면 명령 입력으로 오인할 수 있다. */
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
    if (stdin_flags < 0 || (stdin_flags & O_ACCMODE) == O_WRONLY)
    {
        if (stdin_flags >= 0)
            errno = EBADF;
        perror("stdin");
        return EXIT_FAILURE;
    }
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
    if (controller.device.dry_run)
    {
        puts("DRY-RUN: hardware access disabled");
    }
    else
    {
        controller.device.fd = open(device, O_RDWR | O_CLOEXEC);
        if (controller.device.fd < 0)
        {
            perror(device);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("DEVICE %s\n", device);
    }
    if (fan_device_get(&controller.device, &state) < 0)
    {
        perror("get status");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (controller.sensor_enabled && !controller.device.dry_run)
    {
        char program[PATH_MAX];
        ssize_t length = readlink("/proc/self/exe", program, sizeof(program) - 1);
        if (length < 0 || (size_t)length == sizeof(program) - 1)
        {
            perror("resolve sensor program");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        program[length] = '\0';
        char *slash = strrchr(program, '/');
        const char *name = "bmp180-monitor";
        if (!slash || (size_t)(slash + 1 - program) + strlen(name) >= sizeof(program))
        {
            fputs("ERR sensor program path too long\n", stderr);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        strcpy(slash + 1, name);
        if (sensor_start(&controller.sensor, program, sensor_bus) < 0)
        {
            perror("start sensor process");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("SENSOR_PROCESS pid=%ld bus=%s address=0x77\n", (long)controller.sensor.pid, sensor_bus);
    }
    /* worker thread를 만들기 전에 센서 process를 fork한다. */
    if (log_path)
    {
        if (event_log_start(&controller.log, log_path) < 0)
        {
            perror("start event log");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        puts("LOG enabled=1 queue_capacity=128");
        event_log_submit(controller.log, "START dry_run=%u sensor=%u encoder=%u lcd=%u",
                         controller.device.dry_run, controller.sensor_enabled, encoder_enabled, lcd_enabled);
    }
    if (lcd_enabled)
    {
        if (lcd_start(&controller.lcd, lcd_bus, lcd_address, controller.device.dry_run) < 0)
        {
            perror("start LCD worker");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("LCD_CONFIG bus=%s address=0x%02x mapping=RS0/RW1/E2/BL3/D4..7\n",
               lcd_bus, lcd_address);
    }
    if (controller_print_speed(&controller) < 0)
    {
        perror("get speed");
        result = EXIT_FAILURE;
        goto cleanup;
    }
    if (encoder_enabled && !controller.device.dry_run)
    {
        struct smartfan_speed_status speed;
        if (fan_device_get_speed(&controller.device, &speed) < 0 ||
            !(speed.capabilities & SMARTFAN_CAP_PWM))
        {
            fputs("ERR encoder requires stage 2 PWM DT and driver\n", stderr);
            result = EXIT_FAILURE;
            goto cleanup;
        }
        if (encoder_open(&encoder, encoder_chip, reverse_encoder) < 0)
        {
            perror("open encoder");
            result = EXIT_FAILURE;
            goto cleanup;
        }
        printf("ENCODER %s S1=%u S2=%u debounce_us=%u reverse=%u\n", encoder.chip_path,
               ENCODER_S1_OFFSET, ENCODER_S2_OFFSET, ENCODER_DEBOUNCE_US, reverse_encoder);
    }
    heartbeat_interval = state.lease_ms / 4U;
    if (heartbeat_interval > 250U)
        heartbeat_interval = 250U;
    if (heartbeat_interval < 50U)
        heartbeat_interval = 50U;
    printf("READY lease_ms=%" PRIu32 " max_on_ms=%" PRIu32 "\n",
           (uint32_t)state.lease_ms, (uint32_t)state.max_on_ms);
    struct termios terminal;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &terminal) == 0)
        printf("INPUT tty=1 canonical=%u icrnl=%u eol=CR/LF\n",
               !!(terminal.c_lflag & ICANON), !!(terminal.c_iflag & ICRNL));
    else
        puts("INPUT tty=0 eol=CR/LF");
    controller_print_state(&state);
    known_running = state.running != 0;

    while (!done && !received_signal)
    {
        struct pollfd descriptors[4] = {
            {.fd = STDIN_FILENO, .events = POLLIN},
            {.fd = wake_pipe[0], .events = POLLIN},
            {.fd = encoder.fd, .events = POLLIN},
            {.fd = controller.sensor.fd, .events = POLLIN},
        };
        uint64_t now = fan_now_ms();
        pid_t sensor_pid = controller.sensor.pid;
        sensor_reap(&controller.sensor);
        if (controller.sensor.exited && !sensor_exit_reported)
        {
            int status = controller.sensor.wait_status;
            if (WIFEXITED(status))
                printf("SENSOR_EXIT pid=%ld exit_code=%d\n", (long)sensor_pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("SENSOR_EXIT pid=%ld signal=%d\n", (long)sensor_pid, WTERMSIG(status));
            event_log_submit(controller.log, "SENSOR_EXIT pid=%ld wait_status=%d", (long)sensor_pid, status);
            sensor_exit_reported = true;
        }
        if (fan_device_get(&controller.device, &state) < 0)
        {
            perror("get status");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (known_running && !state.running)
        {
            event_log_submit(controller.log, "MOTOR_STOP reason=%u", (unsigned int)state.stop_reason);
            controller_print_state(&state);
        }
        struct event_log_stats log_stats;
        event_log_snapshot(controller.log, &log_stats);
        if (!log_warned && (log_stats.error || log_stats.dropped))
        {
            fprintf(stderr, "WARN LOG error=%d dropped=%" PRIu64 "; motor control continues\n",
                    log_stats.error, log_stats.dropped);
            log_warned = true;
        }
        known_running = state.running != 0;
        if (controller_auto_tick(&controller) < 0)
        {
            perror("AUTO control");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        controller_report_sensor_transition(&controller);
        if (state.running && now >= next_heartbeat)
        {
            if (fan_device_heartbeat(&controller.device) < 0)
            {
                if (errno != EPIPE && errno != ETIMEDOUT)
                {
                    perror("heartbeat");
                    result = EXIT_FAILURE;
                    exit_reason = "error";
                    break;
                }
            }
            next_heartbeat = fan_now_ms() + heartbeat_interval;
        }
        if (controller_monitor_tick(&controller) < 0)
        {
            perror("monitor");
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (controller.lcd)
        {
            struct smartfan_speed_status speed;
            struct smartfan_led_status leds;
            if (fan_device_get_speed(&controller.device, &speed) < 0 ||
                fan_device_get_leds(&controller.device, &leds) < 0 ||
                fan_device_get(&controller.device, &state) < 0)
            {
                perror("get LCD status");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            lcd_publish_environment(controller.lcd, state.running != 0, speed.level,
                                    !state.running && leds.mode == SMARTFAN_LED_AUTO ? 0 : leds.count,
                                    controller.auto_mode, controller.sensor_enabled, controller_sensor_fresh(&controller),
                                    controller.sample.temperature_mc);
            int error = lcd_error(controller.lcd);
            if (error && !lcd_warned)
            {
                fprintf(stderr, "WARN LCD disabled: %s; motor control continues\n", strerror(error));
                if (error == EOPNOTSUPP && !strcmp(lcd_bus, "/dev/i2c-7"))
                    fputs("LCD requires header I2C at 100kHz: install the prepared LCD DT and reboot.\n", stderr);
                lcd_warned = true;
            }
        }
        int ready = poll(descriptors, 4, 50);
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
        if (descriptors[3].revents)
        {
            for (unsigned int batch = 0; batch < 8; ++batch)
            {
                struct sensor_sample sample;
                int response = sensor_next(&controller.sensor, &sample, fan_now_ms());
                if (!response)
                    break;
                if (response < 0)
                {
                    if (errno != EPIPE || !controller.have_sample || !controller.sample.error)
                        controller.sample.error = errno;
                    controller.have_sample = true;
                    close(controller.sensor.fd);
                    controller.sensor.fd = -1;
                    break;
                }
                ++controller.stats.sensor_frames;
                controller.sample = sample;
                controller.have_sample = true;
            }
            if (controller_auto_tick(&controller) < 0)
            {
                perror("AUTO sensor event");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
        }
        if (descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fputs("ERR encoder unavailable\n", stderr);
            result = EXIT_FAILURE;
            exit_reason = "error";
            break;
        }
        if (descriptors[2].revents & POLLIN)
        {
            int delta;
            uint64_t gaps = encoder.sequence_gaps;
            if (encoder_read(&encoder, &delta) < 0)
            {
                perror("encoder");
                result = EXIT_FAILURE;
                exit_reason = "error";
                break;
            }
            if (encoder.sequence_gaps != gaps)
                fputs("WARN encoder event gap: discarded partial cycle and resynchronized\n", stderr);
            /* 합계가 0인 batch도 STOP 경계를 지날 수 있으므로 step을 순서대로 적용한다. */
            for (unsigned int i = 0; i < encoder.num_steps && !received_signal; ++i)
            {
                if (encoder.steps[i] > 0)
                    ++controller.stats.encoder_cw;
                else
                    ++controller.stats.encoder_ccw;
                if (controller.auto_mode)
                    continue;
                if (controller_change_level(&controller, encoder.steps[i]) < 0 || controller_print_speed(&controller) < 0)
                {
                    perror("encoder speed");
                    result = EXIT_FAILURE;
                    exit_reason = "error";
                    done = true;
                    break;
                }
            }
            if (done || received_signal)
                break;
        }
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
                /* ICRNL이 꺼진 PTY는 Enter를 CR로 전달한다. CRLF의 두 번째 빈 줄은 명령을 중복 실행하지 않는다. */
                if (bytes[i] == '\n' || bytes[i] == '\r')
                {
                    if (discarding)
                    {
                        fprintf(stderr, "ERR command too long or contains NUL\n");
                    }
                    else
                    {
                        line[used] = '\0';
                        int response = controller_command(&controller, line, &known_running,
                                                          encoder_enabled, reverse_encoder, &received_signal);
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
    if (fan_device_set(&controller.device, false) < 0)
    {
        perror("request OFF");
        result = EXIT_FAILURE;
    }
    else
    {
        printf("CLOSED state=OFF reason=%s\n", exit_reason);
    }
cleanup:
    encoder_close(&encoder);
    if (controller.device.fd >= 0)
        close(controller.device.fd);
    sensor_stop(&controller.sensor);
    if (controller.sensor.pid > 0)
        result = EXIT_FAILURE; /* 자식이 남아 있으면 정리가 성공한 것이 아니다. */
    int lcd_cleanup_error = lcd_finish(controller.lcd);
    if (lcd_cleanup_error)
    {
        fprintf(stderr, "WARN LCD shutdown: %s; motor fd already closed\n", strerror(lcd_cleanup_error));
        result = EXIT_FAILURE;
    }
    if (controller.log)
    {
        struct event_log_stats stats;
        event_log_submit(controller.log, "SESSION_STATS commands=%" PRIu64 " frames=%" PRIu64 " faults=%" PRIu64 " recoveries=%" PRIu64,
                         controller.stats.commands, controller.stats.sensor_frames,
                         controller.stats.sensor_faults, controller.stats.sensor_recoveries);
        event_log_submit(controller.log, "STOP reason=%s result=%d", received_signal ? "signal" : exit_reason, result);
        int error = event_log_finish(controller.log, &stats);
        printf("LOG_CLOSED written=%" PRIu64 " dropped=%" PRIu64 " error=%d\n",
               stats.written, stats.dropped, error);
        if (error)
        {
            fprintf(stderr, "WARN LOG shutdown: %s; motor fd already closed\n", strerror(error));
            result = EXIT_FAILURE;
        }
    }
    /* handler가 재사용된 fd에 쓰지 않도록 신호를 차단한 뒤 self-pipe를 닫는다. */
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
