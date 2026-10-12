/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_ENCODER_FILTER_H
#define SMARTFAN_ENCODER_FILTER_H
#include "encoder.h"

/* Diagnostic sampled-state filter; no GPIO/IRQ access here. */
struct encoder_filter
{
    struct encoder_decoder decoder;
    uint64_t stable_ns, candidate_since_ns, last_sample_ns;
    uint64_t raw_changes, accepted_changes, gap_resets;
    unsigned int candidate;
    bool ready;
};
void encoder_filter_init(struct encoder_filter *filter, unsigned int state,
                         uint64_t now_ns, uint64_t stable_ns);
/* Returns a full-cycle +/-1 step. Long sampling gaps discard partial cycles. */
int encoder_filter_sample(struct encoder_filter *filter, unsigned int state,
                          uint64_t now_ns);
#endif
