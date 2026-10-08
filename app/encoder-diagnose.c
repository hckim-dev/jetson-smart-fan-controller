/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Independent of encoder.c: count kernel records and sample the same requested
 * inputs, without Gray decoding, sequence-gap recovery or timestamp filtering.
 * No output request, module load, DT installation or register writes.
 */
static volatile sig_atomic_t stopped;
static void stop(int signum) { stopped = signum; }

static double monotonic_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}

static void show_irqs(const char *phase)
{
    char *line = NULL;
    size_t capacity = 0;
    FILE *file = fopen("/proc/interrupts", "r");
    bool found = false;
    if (!file) { perror("/proc/interrupts"); return; }
    while (getline(&line, &capacity, file) >= 0) {
        if (strstr(line, "smartfan-diag")) {
            printf("IRQ_%s %s", phase, line);
            found = true;
        }
    }
    if (!found) printf("IRQ_%s no matching IRQ entries\n", phase);
    free(line);
    fclose(file);
}

static void show_pinconf(void)
{
    char *line = NULL;
    size_t capacity = 0;
    unsigned int remaining = 0;
    FILE *file = fopen("/sys/kernel/debug/pinctrl/2430000.pinmux/pinconf-groups", "r");
    if (!file) { printf("PINCONF unavailable: %s (run with sudo)\n", strerror(errno)); return; }
    while (getline(&line, &capacity, file) >= 0) {
        if (strstr(line, "soc_gpio41_ph7") || strstr(line, "soc_gpio43_pi1") ||
            strstr(line, "spi3_sck_py0"))
            remaining = 15;
        if (remaining) { fputs(line, stdout); --remaining; }
    }
    free(line);
    fclose(file);
}

static uint32_t be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Check the live DT resource before mapping known Tegra234 control registers.
 * Security bank 0x2200000 is NOT the GPIO control bank 0x2210000.
 */
static void *map_gpio_registers(void)
{
    unsigned char reg[32];
    char compatible[64] = {0};
    FILE *file = fopen("/proc/device-tree/bus@0/gpio@2200000/compatible", "rb");
    if (!file) return MAP_FAILED;
    size_t length = fread(compatible, 1, sizeof(compatible) - 1, file);
    fclose(file);
    if (!length || strcmp(compatible, "nvidia,tegra234-gpio")) return MAP_FAILED;
    file = fopen("/proc/device-tree/bus@0/gpio@2200000/reg", "rb");
    if (!file) return MAP_FAILED;
    length = fread(reg, 1, sizeof(reg), file);
    fclose(file);
    if (length != sizeof(reg) || be32(reg + 16) != 0 ||
        be32(reg + 20) != 0x02210000 || be32(reg + 24) != 0 ||
        be32(reg + 28) != 0x10000) {
        fputs("REGISTERS skipped: unexpected live GPIO resource\n", stdout);
        return MAP_FAILED;
    }
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        printf("REGISTERS unavailable: %s (run with sudo)\n", strerror(errno));
        return MAP_FAILED;
    }
    void *mapping = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, 0x02210000);
    int saved_errno = errno;
    close(fd);
    if (mapping == MAP_FAILED) printf("REGISTERS unavailable: %s\n", strerror(saved_errno));
    return mapping;
}

static void show_registers(void *mapping, const char *phase)
{
    /* Tegra234 H = bank4/port1, pin7; I = bank4/port2, pin1.
     * bank*0x1000 + port*0x200 + pin*0x20 (gpio-tegra186.c).
     */
    static const unsigned int offsets[] = {0x42e0, 0x4420};
    if (mapping == MAP_FAILED) return;
    for (unsigned int i = 0; i < 2; ++i) {
        const volatile uint32_t *r = (const volatile uint32_t *)
            ((const unsigned char *)mapping + offsets[i]);
        uint32_t config = r[0], debounce = r[1], input = r[2], output = r[3];
        printf("REG_%s S%u config=0x%08" PRIx32 " enable=%u output=%u "
               "trigger=%u debounce_enable=%u debounce_threshold=%u irq_enable=%u "
               "input=%u output_floated=%u\n", phase, i + 1, config,
               config & 1U, (config >> 1) & 1U, (config >> 2) & 3U,
               (config >> 5) & 1U, debounce & 255U, (config >> 6) & 1U,
               input & 1U, output & 1U);
    }
}

int main(int argc, char **argv)
{
    unsigned int duration = 20;
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: encoder-diagnose [seconds: 1..120]\n"
             "J12 S1=12/PH.07/50, S2=38/PI.01/52; GPIO input requests only.");
        return EXIT_SUCCESS;
    }
    if (argc > 2) return EXIT_FAILURE;
    if (argc == 2) {
        char *end;
        errno = 0;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (errno || *end || end == argv[1] || parsed < 1 || parsed > 120) {
            fputs("Duration must be 1..120 seconds\n", stderr);
            return EXIT_FAILURE;
        }
        duration = (unsigned int)parsed;
    }
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction"); return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int chip = open("/dev/gpiochip0", O_RDONLY | O_CLOEXEC);
    if (chip < 0) { perror("gpiochip0"); return EXIT_FAILURE; }
    struct gpiochip_info ci = {0};
    if (ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &ci) < 0 ||
        strcmp(ci.label, "tegra234-gpio")) {
        fputs("Expected tegra234-gpio at gpiochip0\n", stderr);
        close(chip); return EXIT_FAILURE;
    }
    const unsigned int offsets[] = {ENCODER_S1_OFFSET, ENCODER_S2_OFFSET};
    const char *names[] = {"PH.07", "PI.01"};
    for (unsigned int i = 0; i < 2; ++i) {
        struct gpio_v2_line_info info = {.offset = offsets[i]};
        if (ioctl(chip, GPIO_V2_GET_LINEINFO_IOCTL, &info) < 0 || strcmp(info.name, names[i])) {
            fputs("GPIO line name mismatch; no inputs requested\n", stderr);
            close(chip); return EXIT_FAILURE;
        }
    }
    void *registers = map_gpio_registers();
    show_registers(registers, "BEFORE");
    struct gpio_v2_line_request request = {
        .offsets = {ENCODER_S1_OFFSET, ENCODER_S2_OFFSET},
        .consumer = "smartfan-diag",
        .config = {
            .flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                     GPIO_V2_LINE_FLAG_EDGE_FALLING,
            .num_attrs = 1,
            .attrs = {{.attr = {.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE,
                                .debounce_period_us = 0}, .mask = 3}},
        },
        .num_lines = 2, .event_buffer_size = 256,
    };
    int result = EXIT_FAILURE;
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        perror("GPIO_V2_GET_LINE_IOCTL (close other encoder programs)");
        close(chip); goto unmap;
    }
    close(chip);
    if (fcntl(request.fd, F_SETFL, O_NONBLOCK) < 0) { perror("fcntl"); goto close_line; }
    struct gpio_v2_line_values values = {.mask = 3};
    if (ioctl(request.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
        perror("initial GPIO read"); goto close_line;
    }
    uint64_t previous = values.bits & 3U, samples = 0, changes[2] = {0};
    uint64_t rising[2] = {0}, falling[2] = {0}, gaps = 0;
    uint32_t last_seq = 0;
    unsigned int seen = 1U << previous;
    printf("START seconds=%u S1=J12/12:50 S2=J12/38:52 A=%u B=%u "
           "debounce_us_requested=0 (hardware state shown in REG_ACTIVE)\n",
           duration, (unsigned int)(previous & 1U), (unsigned int)((previous >> 1) & 1U));
    puts("손잡이만 양방향으로 천천히 계속 돌려주세요. 진단은 자동 종료됩니다.");
    show_pinconf();
    show_registers(registers, "ACTIVE");
    show_irqs("START");
    double start = monotonic_seconds(), next_report = start + 1;
    result = EXIT_SUCCESS;
    while (!stopped && monotonic_seconds() - start < duration) {
        struct pollfd pfd = {.fd = request.fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fputs("GPIO poll error\n", stderr); result = EXIT_FAILURE; break;
        }
        if (pfd.revents & POLLIN) {
            struct gpio_v2_line_event events[32];
            ssize_t count = read(request.fd, events, sizeof(events));
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0 || (size_t)count % sizeof(events[0])) {
                fputs("GPIO event read error\n", stderr); result = EXIT_FAILURE; break;
            }
            for (size_t i = 0; i < (size_t)count / sizeof(events[0]); ++i) {
                struct gpio_v2_line_event *event = &events[i];
                unsigned int line = event->offset == offsets[0] ? 0 : 1;
                if ((event->offset != offsets[0] && event->offset != offsets[1]) ||
                    (event->id != GPIO_V2_LINE_EVENT_RISING_EDGE &&
                     event->id != GPIO_V2_LINE_EVENT_FALLING_EDGE)) {
                    fputs("Unexpected GPIO event record\n", stderr); result = EXIT_FAILURE; stopped = 1; break;
                }
                if (event->seqno != last_seq + 1U) ++gaps;
                last_seq = event->seqno;
                if (event->id == GPIO_V2_LINE_EVENT_RISING_EDGE) ++rising[line];
                else ++falling[line];
            }
        }
        if (ioctl(request.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0) {
            perror("GPIO read"); result = EXIT_FAILURE; break;
        }
        ++samples;
        uint64_t current = values.bits & 3U;
        changes[0] += (current ^ previous) & 1U;
        changes[1] += ((current ^ previous) >> 1) & 1U;
        previous = current;
        seen |= 1U << current;
        double now = monotonic_seconds();
        if (now >= next_report) {
            printf("STATUS t=%.1f A=%u B=%u sampled_changes=%" PRIu64 "/%" PRIu64
                   " edges=%" PRIu64 "/%" PRIu64 "\n", now - start,
                   (unsigned int)(current & 1U), (unsigned int)((current >> 1) & 1U),
                   changes[0], changes[1], rising[0] + falling[0], rising[1] + falling[1]);
            next_report = now + 1;
        }
    }
    show_irqs("END");
    show_registers(registers, "END");
    printf("SUMMARY samples=%" PRIu64 " sampled_changes_A=%" PRIu64
           " sampled_changes_B=%" PRIu64 " rising_A=%" PRIu64 " falling_A=%" PRIu64
           " rising_B=%" PRIu64 " falling_B=%" PRIu64 " sequence_gaps=%" PRIu64
           " seen_AB=", samples, changes[0], changes[1], rising[0], falling[0], rising[1], falling[1], gaps);
    for (unsigned int state = 0; state < 4; ++state)
        if (seen & (1U << state)) printf("%u%u,", state & 1U, (state >> 1) & 1U);
    puts(" (1ms sampling can miss brief pulses; this is not an oscilloscope)");
close_line:
    close(request.fd);
unmap:
    if (registers != MAP_FAILED) munmap(registers, 0x10000);
    return result;
}
