#include "sdio.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int  sdio_dev_open(device *self);
static int  sdio_dev_close(device *self);
static int  sdio_dev_read(device *self, void *buf, size_t len);
static int  sdio_dev_write(device *self, const void *buf, size_t len);
static int  sdio_dev_ioctl(device *self, int cmd, void *arg);
static int  sdio_stream_read(stream_device *self, void *buf, size_t len);
static int  sdio_stream_write(stream_device *self, const void *buf, size_t len);
static int  sdio_stream_flush(stream_device *self);
static int  sdio_sr(stream_device *self, void *b, size_t l, void *m);
static int  sdio_sw(stream_device *self, const void *b, size_t l, const void *m);

static const struct stream_deviceVtable sdio_stream_vtable = {
    .read = sdio_stream_read, .write = sdio_stream_write, .flush = sdio_stream_flush,
    .read_frame = sdio_sr, .write_frame = sdio_sw,
    .transfer_sync = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};
static const struct deviceVtable sdio_dev_vtable = {
    .open = sdio_dev_open, .close = sdio_dev_close,
    .read = sdio_dev_read, .write = sdio_dev_write, .ioctl = sdio_dev_ioctl,
};

device *sdio_create(const void *config)
{
    const sdio_config_t *c = (const sdio_config_t *)config;
    if (!c) return NULL;
    sdio *p = (sdio *)malloc(sizeof(sdio)); if (!p) return NULL;
    memset(p, 0, sizeof(sdio));
    pinmux_port_t sp; uint8_t spn, saf;
    #define RES(s) if(!pinmux_hal_resolve(c->s,&sp,&spn,&saf)){free(p);return NULL;}
    RES(ck_signal); p->ck_port=sp;p->ck_pin=spn;p->ck_af=saf;
    RES(cmd_signal);p->cmd_port=sp;p->cmd_pin=spn;p->cmd_af=saf;
    RES(d0_signal);p->d0_port=sp;p->d0_pin=spn;p->d0_af=saf;
    RES(d1_signal);p->d1_port=sp;p->d1_pin=spn;p->d1_af=saf;
    RES(d2_signal);p->d2_port=sp;p->d2_pin=spn;p->d2_af=saf;
    RES(d3_signal);p->d3_port=sp;p->d3_pin=spn;p->d3_af=saf;
    #undef RES
    p->hal = sdio_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &sdio_dev_vtable;
    p->parent.vtable = &sdio_stream_vtable;
    p->parent.parent.type = DEVICE_TYPE_SDIO;
    p->parent.parent.class = DEVICE_CLASS_STREAM;
    p->parent.parent.name = c->name;
    p->parent.mode = STREAM_MODE_POLL;
    return &p->parent.parent;
}
void sdio_destroy(sdio *self) { if (!self) return; sdio_hal_destroy(self->hal); free(self); }

static int claim_pin(pinmux *pm, pinmux_port_t port, uint8_t pin, uint8_t af, const char *owner)
{
    if (pm->fun->request(pm, port, pin, af, owner)) return -1;
    pinmux_pin_cfg_t cx;
    cx.af = af; cx.mode = 2; cx.otype = 0; cx.speed = 3; cx.pupd = 0;
    pm->fun->config(pm, port, pin, &cx);
    return 0;
}
static int sdio_dev_open(device *self)
{
    sdio *p = (sdio *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        const char *on = p->parent.parent.name;
        if (claim_pin(pm,p->ck_port,p->ck_pin,p->ck_af,on)) return -2;
        if (claim_pin(pm,p->cmd_port,p->cmd_pin,p->cmd_af,on)) return -2;
        if (claim_pin(pm,p->d0_port,p->d0_pin,p->d0_af,on)) return -2;
        if (claim_pin(pm,p->d1_port,p->d1_pin,p->d1_af,on)) return -2;
        if (claim_pin(pm,p->d2_port,p->d2_pin,p->d2_af,on)) return -2;
        if (claim_pin(pm,p->d3_port,p->d3_pin,p->d3_af,on)) return -2;
    }
    sdio_hal_enable_clock(p->hal);
    sdio_hal_power_up(p->hal);
    sdio_hal_set_clock_div(p->hal, 118);
    sdio_hal_set_bus_width(p->hal, 4);
    sdio_hal_enable_ck(p->hal, 1);
    return 0;
}
static int sdio_dev_close(device *self)
{
    sdio *p = (sdio *)self;
    sdio_hal_enable_ck(p->hal, 0); sdio_hal_power_down(p->hal);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

static int sdio_stream_read(stream_device *s, void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }
static int sdio_stream_write(stream_device *s, const void *b, size_t l) { (void)s;(void)b;(void)l;return -1; }
static int sdio_stream_flush(stream_device *self) { (void)self;return 0; }
static int sdio_sr(stream_device *s, void *b, size_t l, void *m) { (void)s;(void)b;(void)l;(void)m;return -1; }
static int sdio_sw(stream_device *s, const void *b, size_t l, const void *m) { (void)s;(void)b;(void)l;(void)m;return -1; }
static int sdio_dev_read(device *s, void *b, size_t l) { return sdio_stream_read((stream_device*)s,b,l); }
static int sdio_dev_write(device *s, const void *b, size_t l) { return sdio_stream_write((stream_device*)s,b,l); }

static int sdio_dev_ioctl(device *self, int cmd, void *arg)
{
    sdio *p = (sdio *)self;
    switch (cmd) {
    case SDIO_IOCTL_CMD: {
        sdio_cmd_t *x = arg; if (!x) return -1;
        return sdio_hal_cmd(p->hal, x->index, x->arg, x->resp_type, x->resp);
    }
    case SDIO_IOCTL_SET_CLOCK: { if (!arg) return -1; sdio_hal_set_clock_div(p->hal, *(uint32_t*)arg); return 0; }
    case SDIO_IOCTL_GET_POWER: if (arg) *(uint32_t*)arg = sdio_hal_get_power(p->hal); return 0;
    case SDIO_IOCTL_GET_CLKCR: if (arg) *(uint32_t*)arg = sdio_hal_get_clkcr(p->hal); return 0;
    case STREAM_IOCTL_SET_MODE: { if (!arg) return -1; p->parent.mode = *(const stream_xfer_mode_t*)arg; return 0; }
    case STREAM_IOCTL_GET_MODE: { if (arg) *(stream_xfer_mode_t*)arg = p->parent.mode; return 0; }
    default: return -1;
    }
}
