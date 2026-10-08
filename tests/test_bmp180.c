/* SPDX-License-Identifier: GPL-2.0 */
#include "bmp180.h"
#include "sensor.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Bosch BMP180 DS000-09, section 3.5 worked example. */
    const struct bmp180_calibration reference = {
        .ac1 = 408,
        .ac2 = -72,
        .ac3 = -14383,
        .ac4 = 32741,
        .ac5 = 32757,
        .ac6 = 23153,
        .b1 = 6190,
        .b2 = 4,
        .mb = -32768,
        .mc = -8711,
        .md = 2868,
    };
    int32_t temperature, pressure;
    assert(bmp180_compensate(&reference, 27898, 23843, 0, &temperature, &pressure) == 0);
    assert(temperature == 15000 && pressure == 69964);
    const uint8_t calibration[22] = {
        0x01,
        0x98,
        0xff,
        0xb8,
        0xc7,
        0xd1,
        0x7f,
        0xe5,
        0x7f,
        0xf5,
        0x5a,
        0x71,
        0x18,
        0x2e,
        0x00,
        0x04,
        0x80,
        0x00,
        0xdd,
        0xf9,
        0x0b,
        0x34,
    };
    struct bmp180_calibration parsed;
    assert(bmp180_parse_calibration(calibration, &parsed) == 0);
    assert(parsed.ac1 == 408 && parsed.ac2 == -72 && parsed.ac3 == -14383);
    assert(parsed.mb == -32768 && parsed.mc == -8711 && parsed.ac4 == 32741);
    assert(bmp180_compensate(&parsed, 27898, 23843, 0, &temperature, &pressure) == 0);
    assert(temperature == 15000 && pressure == 69964);
    uint8_t invalid[22] = {0};
    assert(bmp180_parse_calibration(invalid, &parsed) == -1);
    memset(invalid, 255, sizeof(invalid));
    assert(bmp180_parse_calibration(invalid, &parsed) == -1);
    assert(bmp180_compensate(&reference, 70000, 23843, 0, &temperature, &pressure) == -1);
    assert(bmp180_compensate(&reference, 27898, 23843, 4, &temperature, &pressure) == -1);
    struct bmp180_calibration zero_denominator = reference;
    zero_denominator.md = 0;
    assert(bmp180_compensate(&zero_denominator, reference.ac6, 23843, 0, &temperature, &pressure) == -1);
    uint32_t random = 7;
    for (unsigned int test = 0; test < 10000; ++test)
    {
        for (unsigned int i = 0; i < sizeof(invalid); ++i)
        {
            random = random * 1664525U + 1013904223U;
            invalid[i] = (uint8_t)(random >> 24);
        }
        if (bmp180_parse_calibration(invalid, &parsed) == 0)
        {
            int result = bmp180_compensate(&parsed, random & 65535U,
                                           (random >> 1) & 524287U, 3, &temperature, &pressure);
            if (!result)
            {
                assert(temperature >= -40000 && temperature <= 85000);
                assert(pressure >= 30000 && pressure <= 110000);
            }
        }
    }
    assert(auto_temperature_level(0, 23999) == 0);
    assert(auto_temperature_level(0, 24000) == 1);
    assert(auto_temperature_level(1, 23500) == 1);
    assert(auto_temperature_level(1, 22999) == 0);
    assert(auto_temperature_level(0, 27000) == 2);
    assert(auto_temperature_level(2, 25500) == 2);
    assert(auto_temperature_level(2, 24999) == 1);
    assert(auto_temperature_level(0, 85000) == 5);
    assert(auto_temperature_level(5, -40000) == 0);
    puts("PASS BMP180: Bosch reference vector, endianness/signed coefficients, malformed data, ranges, hysteresis");
    return 0;
}
