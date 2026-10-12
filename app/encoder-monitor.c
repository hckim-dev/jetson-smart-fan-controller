/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"
#include "encoder-filter.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t interrupted;

static void stop_monitor(int signal_number)
{
    interrupted = signal_number;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--chip /dev/gpiochipN] [--reverse] "
                    "[--debounce-us 0..255000 | --no-debounce] [--raw] [--poll-levels]\n"
                    "Software diagnostic: --stable-us 100..100000 [--sample-us 10..10000] [--seconds 1..120]\n"
                    "Default debounce: %uus; --poll-levels uses period 0.\n"
                    "Input pins: S1=J12/12, S2=J12/38; supply=3.3V, shared GND.\n",
            program, ENCODER_DEBOUNCE_US);
}

static int parse_number(const char *text, unsigned int minimum,
                        unsigned int maximum, unsigned int *value)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || errno || *end ||
        parsed < minimum || parsed > maximum)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static int monotonic_ns(uint64_t *now)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1;
    *now = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return 0;
}

static int monitor_stable(struct encoder_input *input, unsigned int stable_us,
                          unsigned int sample_us, unsigned int seconds, bool reverse)
{
    unsigned int a, b;
    uint64_t start, now, previous, samples = 0, cw = 0, ccw = 0, max_gap = 0;
    if (encoder_get_levels(input, &a, &b) < 0 || monotonic_ns(&start) < 0)
        return -1;
    previous = start;
    struct encoder_filter filter;
    encoder_filter_init(&filter, (a << 1U) | b, start, (uint64_t)stable_us * 1000U);
    uint64_t next_report = start + UINT64_C(1000000000);
    uint64_t deadline = seconds ? start + (uint64_t)seconds * UINT64_C(1000000000) : UINT64_MAX;
    const struct timespec delay = {.tv_sec = sample_us / 1000000U,
                                   .tv_nsec = (sample_us % 1000000U) * 1000L};
    printf("SOFT_FILTER chip=%s S1=12 S2=38 stable_us=%u sample_us=%u seconds=%u "
           "hardware_period_us=0 IRQ=unused steps_per_cycle=1 reverse=%u\n",
           input->chip_path, stable_us, sample_us, seconds, reverse);
    puts("Compare 5s idle, 10s clockwise, 10s counterclockwise, 5s idle. Sample timing is best-effort.");
    while (!interrupted)
    {
        if (nanosleep(&delay, NULL) < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (interrupted)
            break;
        if (encoder_get_levels(input, &a, &b) < 0 || monotonic_ns(&now) < 0)
            return -1;
        if (now >= deadline)
            break;
        ++samples;
        uint64_t gap = now - previous;
        if (gap > max_gap)
            max_gap = gap;
        previous = now;
        int step = encoder_filter_sample(&filter, (a << 1U) | b, now);
        if (reverse)
            step = -step;
        if (step > 0)
            ++cw;
        if (step < 0)
            ++ccw;
        /* 입력 잡음이 터미널을 덮지 않도록 초당 한 번만 보고한다. */
        if (now >= next_report)
        {
            double elapsed = (now - start) / 1e9;
            const char *phase = seconds == 30 ? (elapsed < 5 ? "IDLE_START" : elapsed < 15 ? "CW"
                                                                          : elapsed < 25   ? "CCW"
                                                                                           : "IDLE_END")
                                              : "OBSERVE";
            printf("FILTER t=%.1f phase=%s raw_changes=%" PRIu64 " accepted=%" PRIu64
                   " cw=%" PRIu64 " ccw=%" PRIu64 " invalid=%" PRIu64
                   " gap_resets=%" PRIu64 " ready=%u A=%u B=%u\n",
                   elapsed, phase, filter.raw_changes, filter.accepted_changes,
                   cw, ccw, filter.decoder.invalid_transitions, filter.gap_resets,
                   filter.ready, a, b);
            next_report = now + UINT64_C(1000000000);
        }
    }
    if (monotonic_ns(&now) < 0)
        return -1;
    printf("CLOSED FILTER samples=%" PRIu64 " raw_changes=%" PRIu64 " accepted=%" PRIu64
           " cw=%" PRIu64 " ccw=%" PRIu64 " invalid=%" PRIu64 " gap_resets=%" PRIu64
           " elapsed_s=%.3f mean_sample_us=%.1f max_gap_us=%.1f\n",
           samples, filter.raw_changes, filter.accepted_changes, cw, ccw,
           filter.decoder.invalid_transitions, filter.gap_resets, (now - start) / 1e9,
           samples ? (previous - start) / (double)samples / 1000.0 : 0, max_gap / 1000.0);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"chip", required_argument, NULL, 'c'},
        {"reverse", no_argument, NULL, 'r'},
        {"no-debounce", no_argument, NULL, 'n'},
        {"debounce-us", required_argument, NULL, 'd'},
        {"stable-us", required_argument, NULL, 's'},
        {"sample-us", required_argument, NULL, 'i'},
        {"seconds", required_argument, NULL, 't'},
        {"raw", no_argument, NULL, 'R'},
        {"poll-levels", no_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct encoder_input input = {.fd = -1};
    struct sigaction action = {.sa_handler = stop_monitor};
    const char *chip_path = NULL;
    bool reverse = false, raw = false, poll_levels = false;
    bool debounce_selected = false, no_debounce_selected = false;
    unsigned int debounce_us = ENCODER_DEBOUNCE_US;
    unsigned int stable_us = 0, sample_us = 50, seconds = 0;
    bool sample_selected = false, seconds_selected = false;
    uint64_t clockwise = 0, counterclockwise = 0, last_gaps = 0;
    unsigned int level = 1;
    int option, result = EXIT_SUCCESS;

    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1)
    {
        switch (option)
        {
        case 'c':
            chip_path = optarg;
            break;
        case 'r':
            reverse = true;
            break;
        case 'n':
            debounce_us = 0;
            no_debounce_selected = true;
            break;
        case 'd':
        {
            char *end;
            errno = 0;
            unsigned long value = strtoul(optarg, &end, 10);
            if (optarg[0] < '0' || optarg[0] > '9' || errno || *end || value > 255000UL)
            {
                fputs("--debounce-us requires an integer from 0 to 255000\n", stderr);
                return EXIT_FAILURE;
            }
            debounce_us = (unsigned int)value;
            debounce_selected = true;
            break;
        }
        case 'R':
            raw = true;
            break;
        case 's':
            if (parse_number(optarg, 100, 100000, &stable_us))
                goto invalid;
            break;
        case 'i':
            if (parse_number(optarg, 10, 10000, &sample_us))
                goto invalid;
            sample_selected = true;
            break;
        case 't':
            if (parse_number(optarg, 1, 120, &seconds))
                goto invalid;
            seconds_selected = true;
            break;
        case 'p':
            poll_levels = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc || (debounce_selected && (no_debounce_selected || poll_levels)) ||
        (stable_us && (debounce_selected || no_debounce_selected || poll_levels || raw || sample_us > stable_us)) ||
        (!stable_us && (sample_selected || seconds_selected)))
        goto invalid;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0)
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if (((poll_levels || stable_us) ? encoder_open_levels(&input, chip_path) : encoder_open_with_debounce(&input, chip_path, reverse, debounce_us)) < 0)
    {
        perror("encoder_open (GPIOv2 both-edge input)");
        fprintf(stderr, "Check GPIO permissions, input pinmux, free lines and "
                        "GPIOv2/debounce support.\n");
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (stable_us)
    {
        int response = monitor_stable(&input, stable_us, sample_us, seconds, reverse);
        if (response < 0)
            perror("software filter GPIO sampling");
        encoder_close(&input);
        return response < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (poll_levels)
    {
        const struct timespec interval = {.tv_nsec = 1000000L};
        unsigned int previous_a, previous_b;
        uint64_t changes = 0;

        if (encoder_get_levels(&input, &previous_a, &previous_b) < 0)
        {
            perror("get encoder levels");
            encoder_close(&input);
            return EXIT_FAILURE;
        }
        printf("LEVELS chip=%s S1_offset=%u S2_offset=%u interval_ms=1 "
               "A=%u B=%u\n",
               input.chip_path, ENCODER_S1_OFFSET,
               ENCODER_S2_OFFSET, previous_a, previous_b);
        while (!interrupted)
        {
            unsigned int a, b;

            if (nanosleep(&interval, NULL) < 0 && errno != EINTR)
            {
                perror("nanosleep");
                result = EXIT_FAILURE;
                break;
            }
            if (encoder_get_levels(&input, &a, &b) < 0)
            {
                perror("get encoder levels");
                result = EXIT_FAILURE;
                break;
            }
            if (a != previous_a || b != previous_b)
            {
                ++changes;
                printf("LEVEL change=%" PRIu64 " A=%u B=%u\n", changes, a, b);
                previous_a = a;
                previous_b = b;
            }
        }
        printf("CLOSED level_changes=%" PRIu64 "\n", changes);
        encoder_close(&input);
        return result;
    }
    printf("ENCODER chip=%s S1_offset=%u S2_offset=%u debounce_us=%u "
           "A=%u B=%u reverse=%u\n",
           input.chip_path,
           ENCODER_S1_OFFSET, ENCODER_S2_OFFSET, debounce_us,
           input.decoder.state >> 1U, input.decoder.state & 1U, reverse ? 1U : 0U);
    while (!interrupted)
    {
        struct pollfd descriptor = {.fd = input.fd, .events = POLLIN};
        int ready = poll(&descriptor, 1, 100);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll encoder");
            result = EXIT_FAILURE;
            break;
        }
        if (interrupted)
            break;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "ERR encoder event fd unavailable\n");
            result = EXIT_FAILURE;
            break;
        }
        if (descriptor.revents & POLLIN)
        {
            int delta;
            uint64_t before = input.events;
            if (encoder_read(&input, &delta) < 0)
            {
                perror("read encoder");
                result = EXIT_FAILURE;
                break;
            }
            if (raw && input.events != before)
                printf("EDGE batch=%" PRIu64 " total=%" PRIu64
                       " A=%u B=%u\n",
                       input.events - before, input.events,
                       input.decoder.state >> 1U, input.decoder.state & 1U);
            if (input.sequence_gaps != last_gaps)
            {
                fprintf(stderr, "WARN event sequence gap: partial cycle discarded, "
                                "resynchronizing (gaps=%" PRIu64 ")\n",
                        input.sequence_gaps);
                last_gaps = input.sequence_gaps;
            }
            for (unsigned int index = 0; index < input.num_steps; ++index)
            {
                int step = input.steps[index];
                if (step > 0)
                    clockwise += (unsigned int)step;
                else
                    counterclockwise += (unsigned int)-step;
                level = encoder_clamp_level(level, step, 5U);
                printf("STEP delta=%+d cw=%" PRIu64 " ccw=%" PRIu64
                       " level_preview=%u A=%u B=%u\n",
                       step,
                       clockwise, counterclockwise, level,
                       input.decoder.state >> 1U, input.decoder.state & 1U);
            }
        }
    }
    printf("CLOSED events=%" PRIu64 " sequence_gaps=%" PRIu64
           " invalid_transitions=%" PRIu64 "\n",
           input.events,
           input.sequence_gaps, input.decoder.invalid_transitions);
    encoder_close(&input);
    return result;
invalid:
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
}
