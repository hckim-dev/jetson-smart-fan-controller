/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_ENCODER_H
#define SMARTFAN_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#define ENCODER_S1_OFFSET 50U /* J12 physical 12, PH.07. */
#define ENCODER_S2_OFFSET 52U /* J12 physical 38, PI.01. */
#define ENCODER_DEBOUNCE_US 2000U
#define ENCODER_EVENTS_PER_READ 32U

/* State bit 1 = S1/A; bit 0 = S2/B. CW: 00 -> 10 -> 11 -> 01 -> 00. */
struct encoder_decoder
{
    unsigned int state;
    int accumulator;
    uint64_t invalid_transitions;
};

struct encoder_input
{
    int fd;
    struct encoder_decoder decoder;
    bool reverse;
    bool have_seq;
    bool resync_pending;
    uint32_t last_seq;
    uint64_t events;
    uint64_t sequence_gaps;
    uint64_t resync_after_ns;
    int steps[ENCODER_EVENTS_PER_READ];
    unsigned int num_steps;
    char chip_path[64];
};

void encoder_decoder_init(struct encoder_decoder *decoder, unsigned int state);
/* +/-1 only after a complete four-edge cycle; duplicates/bounce emit no step. */
int encoder_decoder_update(struct encoder_decoder *decoder, unsigned int state);
unsigned int encoder_clamp_level(unsigned int level, int delta,
                                 unsigned int max_level);

/* NULL chip_path discovers the exact tegra234-gpio label; no output requests. */
int encoder_open(struct encoder_input *input, const char *chip_path, bool reverse);
/* Diagnostic variant: debounce_us=0 requests a zero period. Tegra L4T36.5.2
 * still sets the hardware debounce-enable bit; this is not proof of bypass.
 */
int encoder_open_with_debounce(struct encoder_input *input, const char *chip_path,
                               bool reverse, unsigned int debounce_us);
/* Diagnostic input-only request; reads levels without relying on IRQ events. */
int encoder_open_levels(struct encoder_input *input, const char *chip_path);
int encoder_get_levels(struct encoder_input *input, unsigned int *a,
                       unsigned int *b);
/* 0: EAGAIN/no event, 1: consumed bounded batch, -1: error. delta is the sum;
 * apply input->steps[0..num_steps) in order when clamping a bounded level.
 */
int encoder_read(struct encoder_input *input, int *delta);
void encoder_close(struct encoder_input *input);

#endif
