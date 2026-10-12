/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_ENCODER_H
#define SMARTFAN_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#define ENCODER_S1_OFFSET 50U /* J12 물리 12번, PH.07. */
#define ENCODER_S2_OFFSET 52U /* J12 물리 38번, PI.01. */
/* 검증된 EC11+NPN 입력은 기간 0을 사용해 이전 1/2ms 설정을 덮어쓴다. */
#define ENCODER_DEBOUNCE_US 0U
#define ENCODER_EVENTS_PER_READ 32U

/* 상태 bit1은 S1/A, bit0은 S2/B이다. CW 순서는 00 → 10 → 11 → 01 → 00. */
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
/* 4개 edge의 완전한 cycle에서만 +/-1을 반환하며 중복·반동은 step을 만들지 않는다. */
int encoder_decoder_update(struct encoder_decoder *decoder, unsigned int state);
unsigned int encoder_clamp_level(unsigned int level, int delta,
                                 unsigned int max_level);

/* chip_path가 NULL이면 정확한 tegra234-gpio label을 찾아 입력만 요청한다. */
int encoder_open(struct encoder_input *input, const char *chip_path, bool reverse);
/* 진단용 debounce_us=0은 기간 0 요청이다. L4T36.5.2의 enable bit 유지 때문에 완전한 bypass로 단정하지 않는다. */
int encoder_open_with_debounce(struct encoder_input *input, const char *chip_path,
                               bool reverse, unsigned int debounce_us);
/* IRQ에 의존하지 않고 level을 읽는 진단용 입력 요청이다. */
int encoder_open_levels(struct encoder_input *input, const char *chip_path);
int encoder_get_levels(struct encoder_input *input, unsigned int *a,
                       unsigned int *b);
/* 반환값은 event 없음 0, 제한된 batch 처리 1, 오류 -1이다. 경계값 적용은 delta 합계 대신 steps를 순서대로 사용한다. */
int encoder_read(struct encoder_input *input, int *delta);
void encoder_close(struct encoder_input *input);

#endif
