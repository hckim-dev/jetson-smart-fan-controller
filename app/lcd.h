/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_LCD_H
#define SMARTFAN_LCD_H
#include <stdbool.h>
#include <stdint.h>

struct lcd_display;
/* Standard PCF8574 backpack: P0=RS, P1=RW, P2=E, P3=backlight,
 * P4..P7=D4..D7. Address and physical mapping require hardware verification.
 */
void lcd_encode_byte(uint8_t value, bool data, uint8_t output[6]);
void lcd_format(bool running, unsigned int level, unsigned int leds,
                char rows[2][17]);
void lcd_format_environment(bool running, unsigned int level, unsigned int leds,
                            bool automatic, bool sensor_enabled, bool temperature_valid,
                            int32_t temperature_mc, char rows[2][17]);
/* Worker owns all I2C calls. Publish coalesces to the latest state and does not
 * wait for I2C, so a stuck display cannot block motor heartbeat delivery.
 */
int lcd_start(struct lcd_display **display, const char *bus, unsigned int address,
              bool dry_run);
void lcd_publish(struct lcd_display *display, bool running, unsigned int level,
                 unsigned int leds);
void lcd_publish_environment(struct lcd_display *display, bool running, unsigned int level,
                             unsigned int leds, bool automatic, bool sensor_enabled,
                             bool temperature_valid, int32_t temperature_mc);
int lcd_error(struct lcd_display *display);
/* Call after stopping the motor. Returns an errno if join exceeds 2 seconds.
 * On error the live worker/object remain owned until process exit; do not free.
 */
int lcd_finish(struct lcd_display *display);
#endif
