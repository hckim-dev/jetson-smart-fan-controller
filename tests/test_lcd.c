/* SPDX-License-Identifier: GPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "lcd.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    uint8_t bytes[6];
    const uint8_t data[] = {0xa9, 0xad, 0xa9, 0x59, 0x5d, 0x59};
    const uint8_t command[] = {0x28, 0x2c, 0x28, 0x88, 0x8c, 0x88};
    lcd_encode_byte(0xa5, true, bytes);
    assert(!memcmp(bytes, data, 6));
    lcd_encode_byte(0x28, false, bytes);
    assert(!memcmp(bytes, command, 6));
    for (unsigned int value = 0; value < 256; ++value) {
        lcd_encode_byte((uint8_t)value, true, bytes);
        for (unsigned int i = 0; i < 6; ++i) assert(!(bytes[i] & 2));
        assert((bytes[0] >> 4) == (value >> 4));
        assert((bytes[3] >> 4) == (value & 15));
        assert((bytes[0] & 4) == 0 && (bytes[1] & 4) != 0 && (bytes[2] & 4) == 0);
    }
    char rows[2][17];
    lcd_format(false, 5, 0, rows);
    assert(!strcmp(rows[0], "FAN OFF MANUAL  "));
    assert(!strcmp(rows[1], "SPEED:5/5 LED:0 "));
    lcd_format(true, 2, 4, rows);
    assert(!strcmp(rows[0], "FAN ON MANUAL   "));
    assert(!strcmp(rows[1], "SPEED:2/5 LED:4 "));
    lcd_format(false, UINT_MAX, UINT_MAX, rows);
    assert(strlen(rows[0]) == 16 && strlen(rows[1]) == 16);
    struct lcd_display *display = NULL;
    assert(lcd_start(&display, "/dev/i2c-7", 0x77, true) == -1 && errno == EINVAL);
    assert(lcd_start(&display, "/dev/i2c-7", 0x27, true) == 0);
    lcd_publish(display, false, 5, 0);
    lcd_finish(display);
    /* This path cannot be a device: verify worker errors/cleanup without I2C. */
    assert(lcd_start(&display, "/dev/null/not-an-i2c-bus", 0x27, false) == 0);
    const struct timespec interval = {.tv_nsec = 1000000L};
    for (unsigned int i = 0; i < 1000 && !lcd_error(display); ++i)
        nanosleep(&interval, NULL);
    assert(lcd_error(display) == ENOTDIR);
    lcd_publish(display, false, 1, 0);
    lcd_finish(display);
    puts("PASS LCD: PCF8574 strobes, nibble order, RW low, row padding, bounds, worker errors/shutdown");
    return 0;
}
