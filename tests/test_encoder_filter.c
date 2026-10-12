/* SPDX-License-Identifier: GPL-2.0 */
#include "encoder-filter.h"
#include <assert.h>
#include <stdio.h>

static int hold(struct encoder_filter *filter, unsigned int state, uint64_t *time)
{
    int sum = 0;
    for (unsigned int i = 0; i < 12; ++i)
    {
        *time += 100;
        sum += encoder_filter_sample(filter, state, *time);
    }
    return sum;
}

int main(void)
{
    struct encoder_filter filter;
    uint64_t time = 0;
    encoder_filter_init(&filter, 0, time, 1000);
    assert(hold(&filter, 0, &time) == 0 && filter.ready);
    /* 짧은 잡음은 decoder에 전달되면 안 된다. */
    time += 100;
    assert(encoder_filter_sample(&filter, 2, time) == 0);
    time += 900;
    assert(encoder_filter_sample(&filter, 0, time) == 0);
    assert(hold(&filter, 0, &time) == 0 && filter.accepted_changes == 0);
    for (unsigned int i = 0; i < 200; ++i)
    {
        time += 100;
        assert(encoder_filter_sample(&filter, i % 2 ? 0 : 2, time) == 0);
    }
    assert(filter.accepted_changes == 0 && filter.raw_changes > 190);
    assert(hold(&filter, 0, &time) == 0);
    int sum = 0;
    const unsigned int cw[] = {2, 3, 1, 0}, ccw[] = {1, 3, 2, 0};
    for (unsigned int i = 0; i < 4; ++i)
        sum += hold(&filter, cw[i], &time);
    assert(sum == 1);
    sum = 0;
    for (unsigned int i = 0; i < 4; ++i)
        sum += hold(&filter, ccw[i], &time);
    assert(sum == -1 && filter.accepted_changes == 8);

    /* 관찰 공백으로 안정성이나 cycle 완료를 증명할 수 없다. */
    assert(hold(&filter, 2, &time) == 0);
    time += 2000;
    assert(encoder_filter_sample(&filter, 3, time) == 0);
    assert(!filter.ready && filter.gap_resets == 1);
    assert(hold(&filter, 3, &time) == 0 && filter.ready);
    assert(hold(&filter, 1, &time) == 0);
    assert(hold(&filter, 0, &time) == 0);
    assert(filter.decoder.accumulator == 2);

    encoder_filter_init(&filter, 0, time, 1000);
    assert(hold(&filter, 0, &time) == 0);
    assert(hold(&filter, 3, &time) == 0);
    assert(filter.decoder.invalid_transitions == 1);
    assert(encoder_filter_sample(&filter, 3, time - 1) == 0);
    assert(!filter.ready && filter.gap_resets == 1);
    time += 100;
    assert(encoder_filter_sample(&filter, 4, time) == 0);
    assert(!filter.ready && filter.gap_resets == 2);
    puts("PASS software encoder filter: short noise, stable CW/CCW, diagonal rejection, sampling gaps, startup/reseed");
    return 0;
}
