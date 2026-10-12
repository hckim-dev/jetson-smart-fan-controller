/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_ENCODER_FILTER_H
#define SMARTFAN_ENCODER_FILTER_H
#include "encoder.h"

/* 진단용 level 안정 시간 필터이며 GPIO/IRQ에 직접 접근하지 않는다. */
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
/* 완전한 cycle에서만 +/-1을 반환한다. 긴 관찰 공백은 부분 cycle을 폐기한다. */
int encoder_filter_sample(struct encoder_filter *filter, unsigned int state,
                          uint64_t now_ns);
#endif
