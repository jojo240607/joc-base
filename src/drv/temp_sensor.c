#include "temp_sensor.h"
#include "devmgr/device_manager.h"   /* resolve the dependency ADC by name */
#include "temp_hal.h"                 /* factory calibration words */
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

/* subclass vtable (defined below; forward-declared so temp_sensor_init can ref it) */
static const struct control_deviceVtable temp_control_vtable;

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

/* one shared vtable for the whole temp-sensor class — assigned by init() */
static const struct deviceVtable temp_dev_vtable = {
    .open  = temp_dev_open,
    .close = temp_dev_close,
    .read  = temp_dev_read,
    .write = temp_dev_write,
    .ioctl = temp_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. The attached ADC is resolved by name
 * through the device manager; the factory calibration words are read here
 * (they live in chip system memory, so they cannot be a compile-time constant
 * in the config). */
device *temp_sensor_create(const void *config)
{
    const temp_config_t *c = (const temp_config_t *)config;
    device *adc = device_manager_get(c->adc_name);
    if (!adc) return NULL;                 /* #2: ADC dependency not registered */
    temp_sensor *self = (temp_sensor *)malloc(sizeof(temp_sensor));
    if (!self) return NULL;
    memset(self, 0, sizeof(temp_sensor));
    self->adc     = adc;
    self->vdda_mv = c->vdda_mv;
    self->ts_cal1 = temp_hal_ts_cal1();
    self->ts_cal2 = temp_hal_ts_cal2();
    self->parent.parent.type = DEVICE_TYPE_TEMP_SENSOR;  /* driver sets its own class */
    self->parent.parent.name = c->name;                   /* driver sets its own name */
    temp_sensor_init(self);
    return (device *)self;
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
    self->parent.parent.vtable = &temp_dev_vtable;       /* base device vtable */
    self->parent.vtable        = &temp_control_vtable;   /* control-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_TEMP_SENSOR;
    self->parent.parent.class  = DEVICE_CLASS_CONTROL;
    self->fun = &temp_sensor_fun;
    /* no hardware to bring up; open()/close() are no-ops */
}

void temp_sensor_deinit(temp_sensor *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
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

/* control-class ops — the REAL implementations; the base deviceVtable forwards
 * here. command() is the ioctl-style control surface; set/get unused -> -1. */
static int temp_control_command(control_device *self, int cmd, void *arg)
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
static int temp_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int temp_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static const struct control_deviceVtable temp_control_vtable = {
    .command = temp_control_command,
    .set     = temp_control_set,
    .get     = temp_control_get,
};

/* base device-interface ops forward to the control-class vtable */
static int temp_dev_ioctl(device *self, int cmd, void *arg)
    { return temp_control_command((control_device *)self, cmd, arg); }

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
