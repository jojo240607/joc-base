#include "temp_sensor.h"
#include <stdlib.h>
#include <string.h>

/* Temperature sensor has a POSITIVE coefficient on this chip:
   raw ADC at 110 C (TS_CAL2) is HIGHER than at 30 C (TS_CAL1).
   Linear interpolation between the two factory points:
       T = 30 + 80 * (TS_CAL1 - TS_DATA) / (TS_CAL1 - TS_CAL2)
   TS_DATA is first scaled to the calibration VDDA (3.3 V). */
#define TS_CAL_VDDA    3.3f

/* virtual implementations dispatched through the unified device vtable */
static int temp_dev_open(device *self);
static int temp_dev_close(device *self);
static int temp_dev_read(device *self, void *buf, size_t len);
static int temp_dev_write(device *self, const void *buf, size_t len);
static int temp_dev_ioctl(device *self, int cmd, void *arg);
static float temp_vread_celsius(temp_sensor *t);

/* public methods — `static`, reachable ONLY through self->fun-> */
static float temp_sensor_read_celsius(temp_sensor *self);
static int32_t temp_sensor_read_celsius_x10(temp_sensor *self);

const struct temp_sensorFun temp_sensor_fun = {
    .destroy = temp_sensor_destroy,
    .init    = temp_sensor_init,
    .deinit  = temp_sensor_deinit,
    .read_celsius     = temp_sensor_read_celsius,
    .read_celsius_x10 = temp_sensor_read_celsius_x10,
};

temp_sensor *temp_sensor_create(device *adc, uint32_t vdda_mv,
                                 uint16_t cal1, uint16_t cal2)
{
    temp_sensor *self = (temp_sensor *)malloc(sizeof(temp_sensor));
    if (!self) return NULL;
    memset(self, 0, sizeof(temp_sensor));
    self->adc     = adc;
    self->vdda_mv = vdda_mv;
    self->ts_cal1 = cal1;
    self->ts_cal2 = cal2;
    temp_sensor_init(self);
    return self;
}

void temp_sensor_destroy(temp_sensor *self)
{
    if (!self) return;
    temp_sensor_deinit(self);
    free(self);
    /* self->adc is owned by the caller, so we do NOT destroy it here */
}

void temp_sensor_init(temp_sensor *self)
{
    if (!self) return;
    device_init(&self->parent);
    self->fun = &temp_sensor_fun;
    self->parent.vtable->open  = temp_dev_open;
    self->parent.vtable->close = temp_dev_close;
    self->parent.vtable->read  = temp_dev_read;
    self->parent.vtable->write = temp_dev_write;
    self->parent.vtable->ioctl = temp_dev_ioctl;
}

void temp_sensor_deinit(temp_sensor *self)
{
    if (!self) return;
    device_deinit(&self->parent);
}

static float temp_sensor_read_celsius(temp_sensor *self)
{
    return self ? temp_vread_celsius(self) : -273.0f;
}

static int32_t temp_sensor_read_celsius_x10(temp_sensor *self)
{
    return self ? (int32_t)(temp_vread_celsius(self) * 10.0f) : -2730;
}

/* --- unified device-interface virtual implementations --- */

static int temp_dev_open(device *self)
{
    (void)self;
    return 0;
}

static int temp_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int temp_dev_read(device *self, void *buf, size_t len)
{
    temp_sensor *t = (temp_sensor *)self;
    if (len < sizeof(float)) return -1;
    *(float *)buf = temp_vread_celsius(t);
    return (int)sizeof(float);
}

static int temp_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;   /* sensor is read-only */
}

static int temp_dev_ioctl(device *self, int cmd, void *arg)
{
    temp_sensor *t = (temp_sensor *)self;
    switch (cmd) {
    case TEMP_IOCTL_READ_X10:
        if (!arg) return -1;
        *(int32_t *)arg = temp_sensor_read_celsius_x10(t);
        return 0;
    case TEMP_IOCTL_SET_VREF_MV:
        if (!arg) return -1;
        t->vdda_mv = *(const uint32_t *)arg;
        return 0;
    case TEMP_IOCTL_GET_CAL1:
        if (!arg) return -1;
        *(uint16_t *)arg = t->ts_cal1;
        return 0;
    case TEMP_IOCTL_GET_CAL2:
        if (!arg) return -1;
        *(uint16_t *)arg = t->ts_cal2;
        return 0;
    default:
        return -1;
    }
}

/* --- core reading: talks to the ADC purely through the device interface --- */
static float temp_vread_celsius(temp_sensor *t)
{
    device *adc = t->adc;

    uint32_t saved = 0;
    adc->vtable->ioctl(adc, ADC_IOCTL_GET_CHANNEL, &saved);
    uint32_t ch = 16U;                       /* temperature sensor (ADC1_IN16) */
    adc->vtable->ioctl(adc, ADC_IOCTL_SET_CHANNEL, &ch);
    uint32_t raw = 0;
    adc->vtable->read(adc, &raw, sizeof(raw));
    adc->vtable->ioctl(adc, ADC_IOCTL_SET_CHANNEL, &saved);   /* restore channel */

    /* scale the reading to the calibration VDDA (3.3 V) */
    float vdda = (float)t->vdda_mv / 1000.0f;
    float ts = (float)raw * TS_CAL_VDDA / vdda;

    float cal1 = (float)t->ts_cal1;
    float cal2 = (float)t->ts_cal2;
    return 30.0f + 80.0f * (cal1 - ts) / (cal1 - cal2);
}
