/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"

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
                    "[--no-debounce] [--raw] [--poll-levels]\n"
                    "Input pins: S1=J12/12, S2=J12/38; supply=3.3V, shared GND.\n",
            program);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"chip", required_argument, NULL, 'c'},
        {"reverse", no_argument, NULL, 'r'},
        {"no-debounce", no_argument, NULL, 'n'},
        {"raw", no_argument, NULL, 'R'},
        {"poll-levels", no_argument, NULL, 'p'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    struct encoder_input input = {.fd = -1};
    struct sigaction action = {.sa_handler = stop_monitor};
    const char *chip_path = NULL;
    bool reverse = false, raw = false, poll_levels = false;
    unsigned int debounce_us = ENCODER_DEBOUNCE_US;
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
            break;
        case 'R':
            raw = true;
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
    if (optind != argc)
    {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0)
    {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    if ((poll_levels ? encoder_open_levels(&input, chip_path) : encoder_open_with_debounce(&input, chip_path, reverse, debounce_us)) < 0)
    {
        perror("encoder_open (GPIOv2 both-edge input)");
        fprintf(stderr, "Check GPIO permissions, input pinmux, free lines and "
                        "GPIOv2/debounce support.\n");
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
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
}
