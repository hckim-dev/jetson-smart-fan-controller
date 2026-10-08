/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_BMP180_H
#define SMARTFAN_BMP180_H
#include <stdint.h>
struct bmp180_calibration
{
    int16_t ac1, ac2, ac3, b1, b2, mb, mc, md;
    uint16_t ac4, ac5, ac6;
};
struct bmp180_device
{
    int fd;
    struct bmp180_calibration calibration;
};
int bmp180_parse_calibration(const uint8_t bytes[22], struct bmp180_calibration *cal);
int bmp180_compensate(const struct bmp180_calibration *cal, uint32_t ut,
                      uint32_t up, unsigned int oss, int32_t *temperature_mc,
                      int32_t *pressure_pa);
int bmp180_open(struct bmp180_device *device, const char *bus);
int bmp180_measure(struct bmp180_device *device, int32_t *temperature_mc,
                   int32_t *pressure_pa);
void bmp180_close(struct bmp180_device *device);
#endif
