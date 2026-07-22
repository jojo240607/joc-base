#include "dac.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */

/* virtual implementations dispatched through the unified device vtable */
static int dac_dev_open(device *self);
static int dac_dev_close(device *self);
static int dac_dev_read(device *self, void *buf, size_t len);
static int dac_dev_write(device *self, const void *buf, size_t len);
static int dac_dev_ioctl(device *self, int cmd, void *arg);

/* stream-class function forward declarations (referenced by the vtable below) */
static int dac_stream_read(stream_device *self, void *buf, size_t len);
static int dac_stream_write(stream_device *self, const void *buf, size_t len);
static int dac_stream_flush(stream_device *self);
static int dac_sr(stream_device *self, void *b, size_t l, void *m);
static int dac_sw(stream_device *self, const void *b, size_t l, const void *m);

static const struct stream_deviceVtable dac_stream_vtable = {
    .read        = dac_stream_read,
    .write       = dac_stream_write,
    .flush       = dac_stream_flush,
    .read_frame  = dac_sr,
    .write_frame = dac_sw,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable dac_dev_vtable = {
    .open  = dac_dev_open,
    .close = dac_dev_close,
    .read  = dac_dev_read,
    .write = dac_dev_write,
    .ioctl = dac_dev_ioctl,
};

device *dac_create(const void *config)
{
    const dac_config_t *c = (const dac_config_t *)config;
    if (!c) return NULL;
    dac *p = (dac *)malloc(sizeof(dac));
    if (!p) return NULL;
    memset(p, 0, sizeof(dac));
    p->hal = dac_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->channel = c->channel;
    p->out_signal = c->out_signal;
    /* Resolve the output signal name up front. */
    if (c->out_signal)
        pinmux_hal_resolve(c->out_signal, &p->port, &p->pin, &p->af);
    p->parent.parent.type   = DEVICE_TYPE_DAC;     /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &dac_dev_vtable;     /* base device vtable */
    p->parent.vtable        = &dac_stream_vtable;  /* stream-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.mode          = STREAM_MODE_POLL;    /* DAC writes are synchronous */
    return (device *)p;
}

void dac_destroy(dac *self)
{
    if (!self) return;
    dac_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int dac_dev_open(device *self)
{
    dac *p = (dac *)self;
    /* Claim + program the analog output pin through the pinmux BEFORE touching
     * the GPIO registers. A conflict makes the request fail and we refuse. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm && p->out_signal) {
        if (pm->fun->request(pm, p->port, p->pin, p->af, p->parent.parent.name) != 0) {
            printf("[dac] %s: pin P%c%d CONFLICT — refused\r\n",
                   p->parent.parent.name, 'A' + p->port, p->pin);
            return -2;
        }
        pinmux_pin_cfg_t cfg = {
            .af = p->af, .mode = 3, .otype = 0, .speed = 0, .pupd = 0
        };
        pm->fun->config(pm, p->port, p->pin, &cfg);   /* mode=3 -> analog */
    }
    dac_hal_enable_clock(p->hal);
    dac_hal_enable_channel(p->hal, p->channel);
    return 0;
}

static int dac_dev_close(device *self)
{
    dac *p = (dac *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* stream-class ops. A DAC is an output stream: one write() outputs one 12-bit
 * sample. The transfer engine is POLL only (DAC writes are synchronous, no IRQ
 * needed); read()/read_frame() are unsupported (use GET_VALUE ioctl to read DOR). */
static int dac_stream_read(stream_device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }   /* DAC is write-only stream */
static int dac_stream_write(stream_device *self, const void *buf, size_t len)
{
    dac *p = (dac *)self;
    if (len < sizeof(uint16_t)) return -1;
    dac_hal_write(p->hal, p->channel, *(const uint16_t *)buf);
    return (int)sizeof(uint16_t);
}
static int dac_stream_flush(stream_device *self) { (void)self; return 0; }
static int dac_sr(stream_device *self, void *b, size_t l, void *m)
    { (void)self; (void)b; (void)l; (void)m; return -1; }
static int dac_sw(stream_device *self, const void *b, size_t l, const void *m)
    { (void)self; (void)b; (void)l; (void)m; return -1; }

/* base device-interface ops forward to the stream-class vtable */
static int dac_dev_read(device *self, void *buf, size_t len)
    { return dac_stream_read((stream_device *)self, buf, len); }
static int dac_dev_write(device *self, const void *buf, size_t len)
    { return dac_stream_write((stream_device *)self, buf, len); }

static int dac_dev_ioctl(device *self, int cmd, void *arg)
{
    dac *p = (dac *)self;
    switch (cmd) {
    case DAC_IOCTL_SET_VALUE: {
        if (!arg) return -1;
        dac_hal_write(p->hal, p->channel, *(const uint16_t *)arg);
        return 0;
    }
    case DAC_IOCTL_GET_VALUE: {
        if (!arg) return -1;
        *(uint16_t *)arg = dac_hal_get_dor(p->hal, p->channel);
        return 0;
    }
    case DAC_IOCTL_GET_CR: {
        if (!arg) return -1;
        *(uint32_t *)arg = dac_hal_get_cr(p->hal);
        return 0;
    }
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        if (m != STREAM_MODE_POLL) return -1;   /* DAC writes are synchronous only */
        p->parent.mode = m;
        return 0;
    }
    case STREAM_IOCTL_GET_MODE: {
        if (!arg) return -1;
        *(stream_xfer_mode_t *)arg = p->parent.mode;
        return 0;
    }
    default:
        return -1;
    }
}
