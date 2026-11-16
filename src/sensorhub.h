/*
 * qnx-sensorhub — shared definitions for the virtual-sensor resource
 * manager (sensorhub) and its client (sensorctl).
 *
 * QNX 8.0, plain C, no third-party dependencies.
 */
#ifndef SENSORHUB_H
#define SENSORHUB_H

#include <stdint.h>
#include <sys/neutrino.h>
#include <devctl.h>     /* __DIOF/__DIOT/__DION: dcmd must encode direction+size,
                           else the resource-manager framework delivers no
                           data (plain integers => nbytes 0 => EINVAL). */

#define SENSOR_DEV0   "/dev/sensor0"   /* temperature */
#define SENSOR_DEV1   "/dev/sensor1"   /* humidity + pressure */

/* devctl(DCMD_SENSOR_SUBSCRIBE) input: where to send the threshold pulse. */
typedef struct {
    int32_t pid;        /* subscriber process id */
    int32_t chid;       /* subscriber channel id */
    int32_t code;       /* pulse code to send (use SENSOR_PULSE_CODE) */
    float   threshold;  /* temperature threshold, degrees C */
} sensor_subscribe_t;

/* devctl(DCMD_SENSOR_GET_STATS) output. */
typedef struct {
    uint64_t reads;     /* successful read() calls served */
    uint64_t devctls;   /* devctl() calls served */
    uint64_t pulses;    /* threshold pulses sent */
    int32_t  rate_ms;   /* current sampling period */
    float    threshold; /* current temperature threshold, degrees C */
    float    temp;      /* last temperature reading */
    float    hum;       /* last humidity reading, % */
    float    press;     /* last pressure reading, hPa */
} sensor_stats_t;

/* devctl command codes. MUST use __DIO* macros so the framework knows the
 * data direction and size; plain integers (0x8000+n) deliver no data.
 * Class 'S' = private sensor class (no collision with system _DCMD_*). */
#define DCMD_SENSOR_GET_RATE    __DIOF('S', 1, int)
#define DCMD_SENSOR_SET_RATE    __DIOT('S', 2, int)
#define DCMD_SENSOR_GET_STATS   __DIOF('S', 3, sensor_stats_t)
#define DCMD_SENSOR_RESET_STATS __DION('S', 4)
#define DCMD_SENSOR_GET_THRESH  __DIOF('S', 5, float)
#define DCMD_SENSOR_SET_THRESH  __DIOT('S', 6, float)
#define DCMD_SENSOR_SUBSCRIBE   __DIOT('S', 7, sensor_subscribe_t)

/* Pulse code the daemon sends when the temperature threshold is crossed. */
#define SENSOR_PULSE_CODE       (_PULSE_CODE_MINAVAIL + 1)

#define SENSOR_DEFAULT_RATE_MS  1000
#define SENSOR_DEFAULT_THRESH   30.0f

#endif /* SENSORHUB_H */
