/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_EVENT_LOG_H
#define SMARTFAN_EVENT_LOG_H
#include <stdint.h>
#define EVENT_LOG_CAPACITY 128U
#define EVENT_LOG_LINE_SIZE 256U
struct event_log;
struct event_log_stats
{
    uint64_t written, dropped;
    unsigned int queued;
    int error;
};
/* 선택형 append-only 파일 기록이다. 센서 fork 후, 모터 ON 전에 시작한다. */
int event_log_start(struct event_log **log, const char *path);
/* 여러 producer의 문자열을 복사한다. 파일 I/O나 queue 공간을 기다리지 않고 포화 시 새 record를 버린다. errno는 보존하며 신호 handler에서는 호출하지 않는다. */
int event_log_submit(struct event_log *log, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void event_log_snapshot(struct event_log *log, struct event_log_stats *stats);
/* producer와 모터를 먼저 정지한 뒤 drain/join한다. 성공 시 객체를 해제하고, 2초 join 기한 실패에는 객체를 유지해 재시도하거나 process 종료로 정리한다. */
int event_log_finish(struct event_log *log, struct event_log_stats *stats);
#endif
