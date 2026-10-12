/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_LCD_H
#define SMARTFAN_LCD_H
#include <stdbool.h>
#include <stdint.h>

struct lcd_display;
/* PCF8574 표준 연결은 P0=RS/P1=RW/P2=E/P3=backlight/P4..7=D4..7이다. 실제 모듈의 주소·배열을 확인해야 한다. */
void lcd_encode_byte(uint8_t value, bool data, uint8_t output[6]);
void lcd_format(bool running, unsigned int level, unsigned int leds,
                char rows[2][17]);
void lcd_format_environment(bool running, unsigned int level, unsigned int leds,
                            bool automatic, bool sensor_enabled, bool temperature_valid,
                            int32_t temperature_mc, char rows[2][17]);
/* I²C는 worker만 수행한다. publish는 최신 snapshot으로 합쳐 전송을 기다리지 않으므로 heartbeat와 분리된다. */
int lcd_start(struct lcd_display **display, const char *bus, unsigned int address,
              bool dry_run);
void lcd_publish(struct lcd_display *display, bool running, unsigned int level,
                 unsigned int leds);
void lcd_publish_environment(struct lcd_display *display, bool running, unsigned int level,
                             unsigned int leds, bool automatic, bool sensor_enabled,
                             bool temperature_valid, int32_t temperature_mc);
int lcd_error(struct lcd_display *display);
/* 모터 정지 후 호출한다. join이 2초를 넘으면 errno 값을 반환하며 살아 있는 worker/객체를 해제하지 않는다. */
int lcd_finish(struct lcd_display *display);
#endif
