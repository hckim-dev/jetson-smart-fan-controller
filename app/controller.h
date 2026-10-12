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

/* Main owns resource lifetime and serializes all controller calls. */
struct controller
{
    struct fan_device device;
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
int controller_print_speed(struct controller *controller);
int controller_auto_tick(struct controller *controller);
int controller_change_level(struct controller *controller, int delta);
void controller_print_state(const struct smartfan_status *state);
int controller_parse_ms(const char *text, uint32_t minimum, uint32_t maximum,
                        uint32_t *value);
/* Returns 1 to quit, -1 on device failure, 0 to continue. stop_signal must
 * point to the main-owned signal flag; no handlers or fd lifetime here. */
int controller_command(struct controller *controller, char *line, bool *known_running,
                       bool encoder_enabled, bool reverse_encoder, const volatile sig_atomic_t *stop_signal);

#endif
