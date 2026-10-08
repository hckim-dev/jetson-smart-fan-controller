/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SMARTFAN_SENSOR_H
#define SMARTFAN_SENSOR_H
#include <stdint.h>
#include <sys/types.h>
#define SENSOR_MAGIC UINT32_C(0x424d5031)
#define SENSOR_MAX_AGE_MS UINT64_C(3000)
struct sensor_sample
{
    uint32_t magic, sequence;
    int32_t error, temperature_mc, pressure_pa;
    uint32_t reserved;
    uint64_t timestamp_ms;
};
_Static_assert(sizeof(struct sensor_sample) == 32, "Sensor IPC frame size");
struct sensor_reader
{
    int fd;
    pid_t pid;
    unsigned int used;
    uint32_t sequence;
    unsigned char buffer[sizeof(struct sensor_sample)];
};
int sensor_start(struct sensor_reader *reader, const char *program, const char *bus);
/* 1=one complete frame, 0=EAGAIN/partial frame, -1=error/EOF. */
int sensor_next(struct sensor_reader *reader, struct sensor_sample *sample, uint64_t now_ms);
void sensor_reap(struct sensor_reader *reader);
void sensor_stop(struct sensor_reader *reader);
unsigned int auto_temperature_level(unsigned int previous, int32_t temperature_mc);
#endif
