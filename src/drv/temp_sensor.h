#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H

#include "iface/device.h"
#include "adc.h"               /* only for the ADC_IOCTL_* command constants */
#include <stdint.h>

/* device-level control commands for the temperature-sensor driver */
#define TEMP_IOCTL_READ_X10   0x01   /* arg: int32_t*  t_x10 (1 decimal, *10) */
#define TEMP_IOCTL_SET_VREF_MV 0x02  /* arg: const uint32_t* vdda_mv */
#define TEMP_IOCTL_GET_CAL1   0x03   /* arg: uint16_t* factory calib @30C */
#define TEMP_IOCTL_GET_CAL2   0x04   /* arg: uint16_t* factory calib @110C */

/*
 * Driver layer — generic on-chip temperature sensor. Platform-independent:
 * it talks to ANY ADC purely through the `device *` interface (no knowledge of
 * the concrete ADC driver or any chip register) and receives the factory
 * calibration words from the board at construction time. Switching chips =
 * only the board supplies different calib values / a different ADC HAL.
 * Implements the unified `device` interface.
 */
typedef struct _temp_sensor temp_sensor;

struct temp_sensorFun {
    void (*destroy)(temp_sensor *self);
    void (*init)(temp_sensor *self);
    void (*deinit)(temp_sensor *self);
    float   (*read_celsius)(temp_sensor *self);
    int32_t (*read_celsius_x10)(temp_sensor *self);
};

struct _temp_sensor {
    device parent;                /* unified interface — MUST be first member */
    const struct temp_sensorFun *fun;
    device *adc;                  /* unified device interface to ANY ADC */
    uint32_t vdda_mv;             /* actual supply in mV (default 3300) */
    uint16_t ts_cal1;             /* factory calib raw @30C */
    uint16_t ts_cal2;             /* factory calib raw @110C */
};

/* The board fills temp_config_t (defined below) as DATA; temp_sensor_create()
 * resolves the attached ADC by name through the device manager and reads the
 * factory calib words itself (they live in chip system memory, so they cannot
 * be a compile-time constant in the config). So this driver stays free of any
 * HAL / register access. */
device *temp_sensor_create(const void *config);
void temp_sensor_destroy(temp_sensor *self);
void temp_sensor_init(temp_sensor *self);
void temp_sensor_deinit(temp_sensor *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; temp_sensor_create() reads
 * it. The attached ADC is given by NAME and resolved via the device manager. */
typedef struct {
    const char *name;       /* logical device name */
    const char *adc_name;   /* attached ADC device (resolved by name) */
    uint32_t vdda_mv;       /* supply voltage in mV */
} temp_config_t;

extern const struct temp_sensorFun temp_sensor_fun;

#endif /* TEMP_SENSOR_H */
