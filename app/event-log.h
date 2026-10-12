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
/* Optional append-only regular-file log. Start after sensor fork, before ON. */
int event_log_start(struct event_log **log, const char *path);
/* Multiple producers; copies data, never waits for file I/O or queue space.
 * Full queue drops the new record. NULL is a no-op. Returns an errno value,
 * preserving the caller's errno. Not async-signal-safe. */
int event_log_submit(struct event_log *log, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void event_log_snapshot(struct event_log *log, struct event_log_stats *stats);
/* Owner must stop producers and motor first. Drains then joins with a 2s
 * MONOTONIC deadline. Successful join frees the object even on write error.
 * Join failure retains the live object until process exit or finish retry. */
int event_log_finish(struct event_log *log, struct event_log_stats *stats);
#endif
