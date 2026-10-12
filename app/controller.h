/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <signal.h>
#include "fan-device.h"
#include "event-log.h"
#include "lcd.h"
#include "sensor.h"

enum sensor_health
{
    SENSOR_PENDING,
    SENSOR_VALID,
    SENSOR_ERROR,
    SENSOR_STALE,
};

/* Session counter는 main만 갱신하며 worker와 공유하지 않는다. */
struct controller_stats
{
    uint64_t started_ms, commands, sensor_frames, injected_samples;
    uint64_t sensor_faults, sensor_recoveries, encoder_cw, encoder_ccw;
    enum sensor_health previous_health;
};

/* main이 자원 수명을 소유하고 모든 controller 호출을 직렬화한다. */
struct controller
{
    struct fan_device device;
    struct controller_stats stats;
    uint32_t monitor_interval_ms;
    uint64_t next_monitor_ms;
    struct lcd_display *lcd;
    struct event_log *log;
    struct sensor_reader sensor;
    struct sensor_sample sample;
    bool sensor_enabled, have_sample, auto_mode, auto_armed;
    bool auto_rearm_pending;
    unsigned int auto_target;
    enum sensor_health reported_sensor_state;
    int reported_sensor_error;
};

bool controller_sensor_fresh(const struct controller *controller);
bool controller_report_sensor_transition(struct controller *controller);
void controller_print_stats(struct controller *controller);
int controller_monitor_tick(struct controller *controller);
int controller_print_speed(struct controller *controller);
int controller_auto_tick(struct controller *controller);
int controller_change_level(struct controller *controller, int delta);
void controller_print_state(const struct smartfan_status *state);
int controller_parse_ms(const char *text, uint32_t minimum, uint32_t maximum,
                        uint32_t *value);
/* 반환값은 종료 1, 장치 오류 -1, 계속 0이다. stop_signal은 main의 신호 flag를 가리킨다. */
int controller_command(struct controller *controller, char *line, bool *known_running,
                       bool encoder_enabled, bool reverse_encoder, const volatile sig_atomic_t *stop_signal);

#endif
