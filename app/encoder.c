/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void encoder_decoder_init(struct encoder_decoder *decoder, unsigned int state)
{
    decoder->state = state & 3U;
    decoder->accumulator = 0;
    decoder->invalid_transitions = 0;
}

int encoder_decoder_update(struct encoder_decoder *decoder, unsigned int state)
{
    static const signed char transitions[16] = {
        0,
        -1,
        1,
        0,
        1,
        0,
        0,
        -1,
        -1,
        0,
        0,
        1,
        0,
        1,
        -1,
        0,
    };
    unsigned int previous = decoder->state;

    if (state > 3U)
    {
        decoder->accumulator = 0;
        ++decoder->invalid_transitions;
        return 0;
    }
    decoder->state = state;
    if ((previous ^ state) == 3U)
    {
        decoder->accumulator = 0;
        ++decoder->invalid_transitions;
        return 0;
    }
    decoder->accumulator += transitions[previous * 4U + state];
    if (decoder->accumulator == 4)
    {
        decoder->accumulator = 0;
        return 1;
    }
    if (decoder->accumulator == -4)
    {
        decoder->accumulator = 0;
        return -1;
    }
    return 0;
}

unsigned int encoder_clamp_level(unsigned int level, int delta,
                                 unsigned int max_level)
{
    int64_t adjusted = (int64_t)level + delta;

    if (adjusted <= 0)
        return 0;
    if ((uint64_t)adjusted >= max_level)
        return max_level;
    return (unsigned int)adjusted;
}

static int clock_ns(uint64_t *value)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) < 0)
        return -1;
    *value = (uint64_t)timestamp.tv_sec * 1000000000ULL +
             (uint64_t)timestamp.tv_nsec;
    return 0;
}

static int reseed(struct encoder_input *input)
{
    struct gpio_v2_line_values values = {.mask = 3U};
    unsigned int state;
    uint64_t invalid_transitions = input->decoder.invalid_transitions;

    if (ioctl(input->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return -1;
    state = ((values.bits & 1U) ? 2U : 0U) |
            ((values.bits & 2U) ? 1U : 0U);
    encoder_decoder_init(&input->decoder, state);
    input->decoder.invalid_transitions = invalid_transitions;
    if (clock_ns(&input->resync_after_ns) < 0)
        return -1;
    /* Drop queued history through this snapshot, even across multiple batches.
     * Movement while reseeding may be lost; it must not produce a false step.
     */
    input->resync_pending = true;
    return 0;
}

static int open_chip(const char *path)
{
    struct gpiochip_info info = {0};
    int chip_fd = open(path, O_RDONLY | O_CLOEXEC);

    if (chip_fd < 0)
        return -1;
    if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &info) < 0)
    {
        int saved_errno = errno;
        close(chip_fd);
        errno = saved_errno;
        return -1;
    }
    if (strncmp(info.label, "tegra234-gpio", sizeof(info.label)) ||
        info.lines <= ENCODER_S1_OFFSET || info.lines <= ENCODER_S2_OFFSET)
    {
        close(chip_fd);
        errno = ENODEV;
        return -1;
    }
    return chip_fd;
}

static int encoder_open_config(struct encoder_input *input, const char *chip_path,
                               bool reverse, unsigned int debounce_us,
                               bool edges)
{
    struct gpio_v2_line_request request = {
        .offsets = {ENCODER_S1_OFFSET, ENCODER_S2_OFFSET},
        .consumer = "smartfan-encoder",
        .config = {
            .flags = GPIO_V2_LINE_FLAG_INPUT |
                     (edges ? GPIO_V2_LINE_FLAG_EDGE_RISING |
                                  GPIO_V2_LINE_FLAG_EDGE_FALLING
                            : 0U),
            /* Explicitly request the period, including zero. Omitting it
             * retains prior hardware configuration. On Tegra L4T36.5.2,
             * zero sets threshold=0 but does not clear debounce-enable.
             */
            .num_attrs = 1,
            .attrs = {{
                .attr = {
                    .id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE,
                    .debounce_period_us = debounce_us,
                },
                .mask = 3U,
            }},
        },
        .num_lines = 2,
        .event_buffer_size = 64,
    };
    int chip_fd = -1, flags, saved_errno, search_errno = ENODEV;

    memset(input, 0, sizeof(*input));
    input->fd = -1;
    input->reverse = reverse;
    if (chip_path)
    {
        if (strlen(chip_path) >= sizeof(input->chip_path))
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        chip_fd = open_chip(chip_path);
        if (chip_fd < 0)
            return -1;
        strcpy(input->chip_path, chip_path);
    }
    else
    {
        for (unsigned int index = 0; index < 16U; ++index)
        {
            char candidate[64];
            snprintf(candidate, sizeof(candidate), "/dev/gpiochip%u", index);
            chip_fd = open_chip(candidate);
            if (chip_fd >= 0)
            {
                strcpy(input->chip_path, candidate);
                break;
            }
            if (errno != ENOENT && errno != ENODEV)
                search_errno = errno;
        }
        if (chip_fd < 0)
        {
            errno = search_errno;
            return -1;
        }
    }
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0)
    {
        saved_errno = errno;
        close(chip_fd);
        errno = saved_errno;
        return -1;
    }
    close(chip_fd);
    input->fd = request.fd;
    flags = fcntl(input->fd, F_GETFL);
    if (flags < 0 || fcntl(input->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
    flags = fcntl(input->fd, F_GETFD);
    if (flags < 0 || fcntl(input->fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        goto fail;
    input->have_seq = true;
    input->last_seq = 0;
    if (reseed(input) < 0)
        goto fail;
    return 0;
fail:
    saved_errno = errno;
    encoder_close(input);
    errno = saved_errno;
    return -1;
}

int encoder_open_with_debounce(struct encoder_input *input, const char *chip_path,
                               bool reverse, unsigned int debounce_us)
{
    return encoder_open_config(input, chip_path, reverse, debounce_us, true);
}

int encoder_open_levels(struct encoder_input *input, const char *chip_path)
{
    return encoder_open_config(input, chip_path, false, 0, false);
}

int encoder_get_levels(struct encoder_input *input, unsigned int *a,
                       unsigned int *b)
{
    struct gpio_v2_line_values values = {.mask = 3U};

    if (ioctl(input->fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return -1;
    *a = (values.bits & 1U) != 0;
    *b = (values.bits & 2U) != 0;
    return 0;
}

int encoder_open(struct encoder_input *input, const char *chip_path, bool reverse)
{
    return encoder_open_with_debounce(input, chip_path, reverse,
                                      ENCODER_DEBOUNCE_US);
}

int encoder_read(struct encoder_input *input, int *delta)
{
    struct gpio_v2_line_event events[ENCODER_EVENTS_PER_READ];
    ssize_t count;

    *delta = 0;
    input->num_steps = 0;
    do
    {
        count = read(input->fd, events, sizeof(events));
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    if (!count || (size_t)count % sizeof(events[0]))
    {
        errno = EIO;
        return -1;
    }
    size_t number = (size_t)count / sizeof(events[0]);
    input->events += number;
    for (size_t index = 0; index < number; ++index)
    {
        const struct gpio_v2_line_event *event = &events[index];
        unsigned int mask, state;

        if (input->have_seq && event->seqno != input->last_seq + 1U)
        {
            ++input->sequence_gaps;
            input->last_seq = events[number - 1U].seqno;
            *delta = 0;
            input->num_steps = 0;
            /* Current batch and older queued state cannot be replayed safely. */
            if (reseed(input) < 0)
                return -1;
            return 1;
        }
        input->have_seq = true;
        input->last_seq = event->seqno;
        if (event->offset == ENCODER_S1_OFFSET)
            mask = 2U;
        else if (event->offset == ENCODER_S2_OFFSET)
            mask = 1U;
        else
        {
            *delta = 0;
            input->num_steps = 0;
            errno = EPROTO;
            return -1;
        }
        if (event->id != GPIO_V2_LINE_EVENT_RISING_EDGE &&
            event->id != GPIO_V2_LINE_EVENT_FALLING_EDGE)
        {
            *delta = 0;
            input->num_steps = 0;
            errno = EPROTO;
            return -1;
        }
        if (input->resync_pending && event->timestamp_ns <= input->resync_after_ns)
            continue;
        input->resync_pending = false;
        state = event->id == GPIO_V2_LINE_EVENT_RISING_EDGE ? input->decoder.state | mask : input->decoder.state & ~mask;
        int step = encoder_decoder_update(&input->decoder, state);
        if (step)
        {
            if (input->reverse)
                step = -step;
            input->steps[input->num_steps++] = step;
            *delta += step;
        }
    }
    return 1;
}

void encoder_close(struct encoder_input *input)
{
    if (input->fd >= 0)
        close(input->fd);
    input->fd = -1;
}
