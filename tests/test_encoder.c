/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include "encoder.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <unistd.h>

static void make_cycle(struct gpio_v2_line_event *events, uint32_t first_seq,
                       bool clockwise)
{
    for (unsigned int edge = 0; edge < 4; ++edge)
    {
        events[edge] = (struct gpio_v2_line_event){
            .timestamp_ns = (uint64_t)first_seq + edge,
            .seqno = first_seq + edge,
            .offset = ((edge % 2U == 0) == clockwise) ? ENCODER_S1_OFFSET : ENCODER_S2_OFFSET,
            .id = edge < 2 ? GPIO_V2_LINE_EVENT_RISING_EDGE : GPIO_V2_LINE_EVENT_FALLING_EDGE,
        };
    }
}

static void test_event_batches(void)
{
    struct gpio_v2_line_event events[36];
    struct encoder_input input = {.fd = -1, .have_seq = true};
    unsigned int level;
    int descriptors[2], delta;

    /* Synthetic pipe only: these tests never open a GPIO device. */
    assert(pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) == 0);
    input.fd = descriptors[0];
    encoder_decoder_init(&input.decoder, 0);
    make_cycle(events, 1, true);
    make_cycle(events + 4, 5, false);
    assert(write(descriptors[1], events, sizeof(events[0]) * 8U) ==
           (ssize_t)(sizeof(events[0]) * 8U));
    assert(encoder_read(&input, &delta) == 1);
    assert(delta == 0 && input.num_steps == 2);
    assert(input.steps[0] == 1 && input.steps[1] == -1);
    level = 5;
    for (unsigned int index = 0; index < input.num_steps; ++index)
        level = encoder_clamp_level(level, input.steps[index], 5);
    assert(level == 4); /* Clamping the sum would incorrectly leave level 5. */

    make_cycle(events, 9, false);
    make_cycle(events + 4, 13, true);
    assert(write(descriptors[1], events, sizeof(events[0]) * 8U) ==
           (ssize_t)(sizeof(events[0]) * 8U));
    assert(encoder_read(&input, &delta) == 1);
    assert(delta == 0 && input.num_steps == 2);
    assert(input.steps[0] == -1 && input.steps[1] == 1);
    level = encoder_clamp_level(1, input.steps[0], 5);
    assert(level == 0); /* The OFF transition must reach the motor controller. */
    level = encoder_clamp_level(level, input.steps[1], 5);
    assert(level == 1);

    input.reverse = true;
    make_cycle(events, 17, true);
    assert(write(descriptors[1], events, sizeof(events[0]) * 4U) ==
           (ssize_t)(sizeof(events[0]) * 4U));
    assert(encoder_read(&input, &delta) == 1);
    assert(delta == -1 && input.num_steps == 1 && input.steps[0] == -1);
    input.reverse = false;

    for (unsigned int cycle = 0; cycle < 9; ++cycle)
        make_cycle(events + cycle * 4U, 21 + cycle * 4U, true);
    assert(write(descriptors[1], events, sizeof(events)) == (ssize_t)sizeof(events));
    assert(encoder_read(&input, &delta) == 1);
    assert(input.num_steps == 8 && delta == 8); /* At most 32 events per call. */
    assert(encoder_read(&input, &delta) == 1);
    assert(input.num_steps == 1 && delta == 1);
    delta = 99;
    assert(encoder_read(&input, &delta) == 0);
    assert(input.num_steps == 0 && delta == 0);

    /* Old queued edges through a reseed snapshot must not become new steps. */
    input.resync_pending = true;
    input.resync_after_ns = 60;
    make_cycle(events, 57, true);
    make_cycle(events + 4, 61, true);
    assert(write(descriptors[1], events, sizeof(events[0]) * 8U) ==
           (ssize_t)(sizeof(events[0]) * 8U));
    assert(encoder_read(&input, &delta) == 1);
    assert(delta == 1 && input.num_steps == 1);

    /* A sequence gap cancels even a completed step earlier in this batch.
     * Reseeding the synthetic pipe fails ENOTTY, rather than inventing GPIO state.
     */
    make_cycle(events, 65, true);
    make_cycle(events + 4, 70, false);
    assert(write(descriptors[1], events, sizeof(events[0]) * 8U) ==
           (ssize_t)(sizeof(events[0]) * 8U));
    assert(encoder_read(&input, &delta) == -1);
    assert(errno == ENOTTY && input.sequence_gaps == 1);
    assert(delta == 0 && input.num_steps == 0);
    close(descriptors[1]);
    encoder_close(&input);
    assert(input.fd == -1);
}

static int feed(struct encoder_decoder *decoder, const unsigned int *states,
                unsigned int count)
{
    int total = 0;
    for (unsigned int index = 0; index < count; ++index)
    {
        total += encoder_decoder_update(decoder, states[index]);
        assert(decoder->accumulator >= -3 && decoder->accumulator <= 3);
    }
    return total;
}

int main(void)
{
    static const unsigned int clockwise[] = {2, 3, 1, 0};
    static const unsigned int counterclockwise[] = {1, 3, 2, 0};
    static const unsigned int bounce[] = {2, 0, 2, 2, 3, 2, 3, 1, 3, 1, 0};
    static const unsigned int reversal[] = {2, 3, 2, 0, 1, 3, 2, 0};
    struct encoder_decoder decoder;

    encoder_decoder_init(&decoder, 0);
    assert(encoder_decoder_update(&decoder, 2) == 0);
    assert(encoder_decoder_update(&decoder, 3) == 0);
    assert(encoder_decoder_update(&decoder, 1) == 0);
    assert(encoder_decoder_update(&decoder, 0) == 1);
    assert(decoder.accumulator == 0);
    assert(feed(&decoder, counterclockwise, 4) == -1);
    assert(feed(&decoder, bounce, sizeof(bounce) / sizeof(bounce[0])) == 1);
    assert(feed(&decoder, reversal, sizeof(reversal) / sizeof(reversal[0])) == -1);

    /* Invalid two-bit jumps cannot carry a partial cycle into a false step. */
    encoder_decoder_init(&decoder, 0);
    assert(encoder_decoder_update(&decoder, 2) == 0);
    assert(encoder_decoder_update(&decoder, 1) == 0);
    assert(decoder.accumulator == 0);
    assert(decoder.invalid_transitions == 1);
    assert(encoder_decoder_update(&decoder, 0) == 0);
    encoder_decoder_init(&decoder, 0);
    assert(encoder_decoder_update(&decoder, 3) == 0);
    assert(decoder.invalid_transitions == 1);
    assert(encoder_decoder_update(&decoder, 4) == 0);
    assert(decoder.invalid_transitions == 2);

    /* Full cycles decode from every starting phase, without a fixed detent bias. */
    static const unsigned int ring[] = {0, 2, 3, 1};
    for (unsigned int start = 0; start < 4; ++start)
    {
        encoder_decoder_init(&decoder, ring[start]);
        for (unsigned int edge = 1; edge <= 4; ++edge)
            assert(encoder_decoder_update(&decoder, ring[(start + edge) % 4U]) ==
                   (edge == 4 ? 1 : 0));
    }
    encoder_decoder_init(&decoder, 0);
    for (unsigned int count = 0; count < 10000; ++count)
    {
        assert(feed(&decoder, clockwise, 4) == 1);
        assert(feed(&decoder, counterclockwise, 4) == -1);
    }
    assert(encoder_clamp_level(0, -1, 3) == 0);
    assert(encoder_clamp_level(3, 1, 3) == 3);
    assert(encoder_clamp_level(1, 1, 3) == 2);
    assert(encoder_clamp_level(2, -1, 3) == 1);
    assert(encoder_clamp_level(2, INT_MIN, 3) == 0);
    assert(encoder_clamp_level(UINT_MAX, INT_MAX, 3) == 3);
    assert(encoder_clamp_level(1, 1, 0) == 0);
    test_event_batches();
    puts("PASS encoder: Gray cycles, bounce, reversal, invalid states, bounds, "
         "ordered event batches, reverse, bounded reads, stale events, sequence gaps");
    return 0;
}
