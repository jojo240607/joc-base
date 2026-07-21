#include "i2c.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int  i2c_dev_open(device *self);
static int  i2c_dev_close(device *self);
static int  i2c_dev_read(device *self, void *buf, size_t len);
static int  i2c_dev_write(device *self, const void *buf, size_t len);
static int  i2c_dev_ioctl(device *self, int cmd, void *arg);
static int  i2c_stream_read(stream_device *self, void *buf, size_t len);
static int  i2c_stream_write(stream_device *self, const void *buf, size_t len);
static int  i2c_stream_flush(stream_device *self);
static int  i2c_stream_read_frame(stream_device *self, void *b, size_t l, void *m);
static int  i2c_stream_write_frame(stream_device *self, const void *b, size_t l, const void *m);
static int  i2c_do_xfer(i2c *p, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len, int is_write);

static const struct stream_deviceVtable i2c_stream_vtable = {
    .read = i2c_stream_read, .write = i2c_stream_write, .flush = i2c_stream_flush,
    .read_frame = i2c_stream_read_frame, .write_frame = i2c_stream_write_frame,
    .transfer_sync = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};
static const struct deviceVtable i2c_dev_vtable = {
    .open = i2c_dev_open, .close = i2c_dev_close,
    .read = i2c_dev_read, .write = i2c_dev_write, .ioctl = i2c_dev_ioctl,
};

device *i2c_create(const void *config)
{
    const i2c_config_t *c = (const i2c_config_t *)config;
    if (!c || !c->scl_signal || !c->sda_signal) return NULL;
    i2c *p = (i2c *)malloc(sizeof(i2c)); if (!p) return NULL;
    memset(p, 0, sizeof(i2c));
    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->scl_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->sda_signal, &dp, &dpn, &daf)) { free(p); return NULL; }
    p->hal = i2c_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &i2c_dev_vtable;
    p->parent.vtable = &i2c_stream_vtable;
    p->parent.parent.type = DEVICE_TYPE_I2C;
    p->parent.parent.class = DEVICE_CLASS_STREAM;
    p->parent.parent.name = c->name;
    p->parent.mode = STREAM_MODE_POLL;
    p->clk_hz = c->clk_hz; p->speed_hz = c->speed_hz;
    p->scl_port = sp; p->scl_pin = spn; p->scl_af = saf;
    p->sda_port = dp; p->sda_pin = dpn; p->sda_af = daf;
    p->current_addr = 0x50;
    return &p->parent.parent;
}
void i2c_destroy(i2c *self) { if (!self) return; i2c_hal_destroy(self->hal); free(self); }

static int i2c_dev_open(device *self)
{
    i2c *p = (i2c *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->scl_port, p->scl_pin, p->scl_af, p->parent.parent.name) != 0 ||
            pm->fun->request(pm, p->sda_port, p->sda_pin, p->sda_af, p->parent.parent.name) != 0) return -2;
        pinmux_pin_cfg_t cfg = { .af = p->scl_af, .mode = 2, .otype = 1, .speed = 3, .pupd = 1 };
        pm->fun->config(pm, p->scl_port, p->scl_pin, &cfg);
        cfg.af = p->sda_af; pm->fun->config(pm, p->sda_port, p->sda_pin, &cfg);
    }
    i2c_hal_enable_clock(p->hal);
    i2c_hal_software_reset(p->hal);
    i2c_hal_config(p->hal, p->clk_hz, p->speed_hz);
    p->ev_irq = i2c_hal_ev_irq_id(p->hal);
    p->er_irq = i2c_hal_er_irq_id(p->hal);
    /* ISRs in i2c_hal.c — placement there keeps layout away from i2c.o */
    if (p->ev_irq >= 0) irq_register(p->ev_irq, i2c_hal_ev_isr, p);
    if (p->er_irq >= 0) irq_register(p->er_irq, i2c_hal_er_isr, p);
    return 0;
}
static int i2c_dev_close(device *self)
{
    i2c *p = (i2c *)self;
    i2c_hal_disable_ev_irq(p->hal); i2c_hal_disable_er_irq(p->hal);
    i2c_hal_set_peripheral_enable(p->hal, 0);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}
static int i2c_dev_read(device *self, void *buf, size_t len) { return i2c_stream_read((stream_device *)self, buf, len); }
static int i2c_dev_write(device *self, const void *buf, size_t len) { return i2c_stream_write((stream_device *)self, buf, len); }

/* Transfer: POLL or IRQ (IRQ path has timeout to avoid hanging without pull-ups) */
static int i2c_do_xfer(i2c *p, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len, int is_write)
{
    if (p->parent.mode == STREAM_MODE_IRQ) {
        p->addr = addr; p->tx_buf = tx; p->rx_buf = rx;
        p->xfer_len = len; p->xfer_pos = 0;
        p->irq_state = 1; /* I2C_S_SB */ p->irq_result = -1;

        p->xfer_done = 0;
        i2c_hal_set_start(p->hal);               /* begin transfer BEFORE enabling NVIC */
        i2c_hal_enable_ev_irq(p->hal);
        i2c_hal_enable_er_irq(p->hal);
        if (p->ev_irq >= 0) i2c_hal_nvic_enable(p->ev_irq);
        if (p->er_irq >= 0) i2c_hal_nvic_enable(p->er_irq);

        volatile uint32_t tmo = 200000U;          /* timeout prevents hang without pull-ups */
        while (!p->xfer_done && tmo--) { }

        i2c_hal_nvic_disable(p->ev_irq);
        i2c_hal_nvic_disable(p->er_irq);
        i2c_hal_disable_ev_irq(p->hal);
        i2c_hal_disable_er_irq(p->hal);
        return p->irq_result;
    }
    if (is_write) return i2c_hal_master_write(p->hal, addr, tx, len);
    else          return i2c_hal_master_read(p->hal, addr, rx, len);
}

static int i2c_stream_read(stream_device *self, void *buf, size_t len)
    { return i2c_do_xfer((i2c *)self, ((i2c *)self)->current_addr, NULL, (uint8_t *)buf, (uint16_t)len, 0); }
static int i2c_stream_write(stream_device *self, const void *buf, size_t len)
    { return i2c_do_xfer((i2c *)self, ((i2c *)self)->current_addr, (const uint8_t *)buf, NULL, (uint16_t)len, 1); }
static int i2c_stream_flush(stream_device *self) { (void)self; return 0; }
static int i2c_stream_read_frame(stream_device *self, void *b, size_t l, void *m) { (void)self;(void)b;(void)l;(void)m; return -1; }
static int i2c_stream_write_frame(stream_device *self, const void *b, size_t l, const void *m) { (void)self;(void)b;(void)l;(void)m; return -1; }

static int i2c_dev_ioctl(device *self, int cmd, void *arg)
{
    i2c *p = (i2c *)self;
    switch (cmd) {
    case I2C_IOCTL_MASTER_WRITE: { i2c_xfer_t *x = arg; if (!x) return -1; x->result = i2c_do_xfer(p, x->addr, x->buf, NULL, x->len, 1); return x->result; }
    case I2C_IOCTL_MASTER_READ:  { i2c_xfer_t *x = arg; if (!x) return -1; x->result = i2c_do_xfer(p, x->addr, NULL, x->buf, x->len, 0); return x->result; }
    case I2C_IOCTL_BUS_SCAN: { i2c_scan_t *s = arg; if (!s) return -1; s->found = 0; memset(s->acks, 0, 128);
        for (int a = 0; a < 128; a++) { if (i2c_do_xfer(p, (uint16_t)a, NULL, NULL, 0, 1) == 0) { s->acks[a] = 1; s->found++; } } return 0; }
    case I2C_IOCTL_SET_SPEED: { if (!arg) return -1; p->speed_hz = *(uint32_t *)arg; i2c_hal_config(p->hal, p->clk_hz, p->speed_hz); return 0; }
    case I2C_IOCTL_SET_ADDR: { if (!arg) return -1; p->current_addr = *(uint16_t *)arg; return 0; }
    case I2C_IOCTL_GET_ADDR: { if (arg) *(uint16_t *)arg = p->current_addr; return 0; }
    case I2C_IOCTL_GET_CCR:   if (arg) *(uint32_t *)arg = i2c_hal_get_ccr(p->hal); return 0;
    case I2C_IOCTL_GET_CR2_FREQ: if (arg) *(uint32_t *)arg = i2c_hal_get_cr2_freq(p->hal); return 0;
    case I2C_IOCTL_GET_CR1:   if (arg) *(uint32_t *)arg = i2c_hal_get_cr1(p->hal); return 0;
    case I2C_IOCTL_GET_BUSY:  if (arg) *(int *)arg = i2c_hal_is_busy(p->hal); return 0;
    case STREAM_IOCTL_SET_MODE: { if (!arg) return -1; p->parent.mode = *(const stream_xfer_mode_t *)arg; return 0; }
    case STREAM_IOCTL_GET_MODE: { if (arg) *(stream_xfer_mode_t *)arg = p->parent.mode; return 0; }
    default: return -1;
    }
}
