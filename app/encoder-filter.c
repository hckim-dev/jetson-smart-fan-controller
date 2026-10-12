/* SPDX-License-Identifier: GPL-2.0 */
#include "encoder-filter.h"

void encoder_filter_init(struct encoder_filter *filter, unsigned int state,
                         uint64_t now_ns, uint64_t stable_ns)
{
    *filter = (struct encoder_filter){.stable_ns = stable_ns,
                                      .candidate_since_ns = now_ns,
                                      .last_sample_ns = now_ns,
                                      .candidate = state};
    encoder_decoder_init(&filter->decoder, state);
}

int encoder_filter_sample(struct encoder_filter *filter, unsigned int state,
                          uint64_t now_ns)
{
    bool changed = state != filter->candidate;
    if (changed)
        ++filter->raw_changes;
    if (state > 3U || now_ns < filter->last_sample_ns ||
        now_ns - filter->last_sample_ns > filter->stable_ns)
    {
        /* Missing observations are not proof that a state stayed stable. */
        ++filter->gap_resets;
        filter->ready = false;
        filter->decoder.accumulator = 0;
        filter->candidate = state;
        filter->candidate_since_ns = now_ns;
        filter->last_sample_ns = now_ns;
        return 0;
    }
    filter->last_sample_ns = now_ns;
    if (changed)
    {
        filter->candidate = state;
        filter->candidate_since_ns = now_ns;
        return 0;
    }
    if (now_ns - filter->candidate_since_ns < filter->stable_ns)
        return 0;
    if (!filter->ready)
    {
        uint64_t invalid = filter->decoder.invalid_transitions;
        encoder_decoder_init(&filter->decoder, state);
        filter->decoder.invalid_transitions = invalid;
        filter->ready = true;
        return 0;
    }
    if (state == filter->decoder.state)
        return 0;
    ++filter->accepted_changes;
    return encoder_decoder_update(&filter->decoder, state);
}
