#include "adc.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int adc_dev_open(device *self);
static int adc_dev_close(device *self);
static int adc_dev_read(device *self, void *buf, size_t len);
static int adc_dev_write(device *self, const void *buf, size_t len);
static int adc_dev_ioctl(device *self, int cmd, void *arg);
static void adc_hw_init(adc *self);

/* public methods — `static`, reachable ONLY through self->fun-> (forward decls
 * so the const fun table below can reference them, per the moban template) */
static uint32_t adc_read(adc *self);
static uint32_t adc_read_mv(adc *self);
static void adc_set_channel(adc *self, uint32_t channel);

const struct adcFun adc_fun = {
    .destroy     = adc_destroy,
    .init        = adc_init,
    .deinit      = adc_deinit,
    .read        = adc_read,
    .read_mv     = adc_read_mv,
    .set_channel = adc_set_channel,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *adc_create(const void *config)
{
    const adc_config_t *c = (const adc_config_t *)config;
    adc *self = (adc *)malloc(sizeof(adc));
    if (!self) return NULL;
    memset(self, 0, sizeof(adc));
    self->hal     = adc_hal_create(c->periph, c->channel);
    self->channel = c->channel;
    self->vdda_mv = c->vdda_mv;
    self->parent.type = DEVICE_TYPE_ADC;     /* driver sets its own class */
    self->parent.name = c->name;             /* driver sets its own name */
    adc_init(self);
    return (device *)self;
}

void adc_destroy(adc *self)
{
    if (!self) return;
    adc_deinit(self);
    free(self);
}

void adc_init(adc *self)
{
    if (!self) return;
    device_init(&self->parent);          /* allocate the unified vtable */
    self->fun = &adc_fun;
    /* implement the unified device interface with the ADC behaviour */
    self->parent.vtable->open  = adc_dev_open;
    self->parent.vtable->close = adc_dev_close;
    self->parent.vtable->read  = adc_dev_read;
    self->parent.vtable->write = adc_dev_write;
    self->parent.vtable->ioctl = adc_dev_ioctl;
    adc_hw_init(self);
}

void adc_deinit(adc *self)
{
    if (!self) return;
    device_deinit(&self->parent);        /* free the unified vtable */
}

static uint32_t adc_read(adc *self)
{
    if (!self) return 0U;
    return adc_hal_single_convert(self->hal);
}

static uint32_t adc_read_mv(adc *self)
{
    if (!self) return 0U;
    return adc_hal_to_mv(adc_hal_single_convert(self->hal), self->vdda_mv);
}

static void adc_set_channel(adc *self, uint32_t channel)
{
    if (!self) return;
    self->channel = channel;
    adc_hal_set_channel(self->hal, channel);
}

/* --- unified device-interface virtual implementations --- */

static int adc_dev_open(device *self)
{
    adc_hw_init((adc *)self);
    return 0;
}

static int adc_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int adc_dev_read(device *self, void *buf, size_t len)
{
    adc *a = (adc *)self;
    if (len < sizeof(uint32_t)) return -1;
    *(uint32_t *)buf = adc_read(a);
    return (int)sizeof(uint32_t);
}

static int adc_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;   /* ADC is read-only */
}

static int adc_dev_ioctl(device *self, int cmd, void *arg)
{
    adc *a = (adc *)self;
    switch (cmd) {
    case ADC_IOCTL_SET_CHANNEL:
        if (!arg) return -1;
        adc_set_channel(a, *(const uint32_t *)arg);
        return 0;
    case ADC_IOCTL_GET_CHANNEL:
        if (!arg) return -1;
        *(uint32_t *)arg = a->channel;
        return 0;
    case ADC_IOCTL_SET_VREF_MV:
        if (!arg) return -1;
        a->vdda_mv = *(const uint32_t *)arg;
        return 0;
    case ADC_IOCTL_READ_MV:
        if (!arg) return -1;
        *(uint32_t *)arg = adc_read_mv(a);
        return 0;
    default:
        return -1;
    }
}

/* --- hardware bring-up (delegated to HAL via the opaque handle) --- */
static void adc_hw_init(adc *self)
{
    adc_hal_enable_clock(self->hal);
    adc_hal_common_config(self->hal);
    adc_hal_config_gpio(self->hal);
    adc_hal_config_channel(self->hal);
}
