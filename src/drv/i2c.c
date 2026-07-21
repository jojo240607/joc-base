#include "i2c.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int i2c_dev_open(device *self);
static int i2c_dev_close(device *self);
static int i2c_dev_read(device *self, void *buf, size_t len);
static int i2c_dev_write(device *self, const void *buf, size_t len);
static int i2c_dev_ioctl(device *self, int cmd, void *arg);
static int i2c_control_command(control_device *self, int cmd, void *arg);
static int i2c_control_set(control_device *self, int param, const void *val);
static int i2c_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable i2c_control_vtable = {
    .command = i2c_control_command, .set = i2c_control_set, .get = i2c_control_get,
};
static const struct deviceVtable i2c_dev_vtable = {
    .open = i2c_dev_open, .close = i2c_dev_close,
    .read = i2c_dev_read, .write = i2c_dev_write, .ioctl = i2c_dev_ioctl,
};

device *i2c_create(const void *config)
{
    const i2c_config_t *c = (const i2c_config_t *)config;
    if (!c || !c->scl_signal || !c->sda_signal) return NULL;
    i2c *p = (i2c *)malloc(sizeof(i2c));
    if (!p) return NULL;
    memset(p, 0, sizeof(i2c));
    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->scl_signal, &sp, &spn, &saf)) { printf("[i2c] %s: unknown SCL\r\n", c->name); free(p); return NULL; }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->sda_signal, &dp, &dpn, &daf)) { printf("[i2c] %s: unknown SDA\r\n", c->name); free(p); return NULL; }
    p->hal = i2c_hal_create(c->peripheral);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.vtable = &i2c_dev_vtable;
    p->parent.vtable = &i2c_control_vtable;
    p->parent.parent.type = DEVICE_TYPE_I2C;
    p->parent.parent.class = DEVICE_CLASS_CONTROL;
    p->parent.parent.name = c->name;
    p->clk_hz = c->clk_hz; p->speed_hz = c->speed_hz;
    p->scl_port = sp; p->scl_pin = spn; p->scl_af = saf;
    p->sda_port = dp; p->sda_pin = dpn; p->sda_af = daf;
    return &p->parent.parent;
}
void i2c_destroy(i2c *self) { if (!self) return; i2c_hal_destroy(self->hal); free(self); }

static int i2c_dev_open(device *self)
{
    i2c *p = (i2c *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->scl_port, p->scl_pin, p->scl_af, p->parent.parent.name) != 0 ||
            pm->fun->request(pm, p->sda_port, p->sda_pin, p->sda_af, p->parent.parent.name) != 0)
            { printf("[i2c] %s: pin conflict\r\n", p->parent.parent.name); return -2; }
        pinmux_pin_cfg_t cfg = { .af = p->scl_af, .mode = 2, .otype = 1, .speed = 3, .pupd = 1 };
        pm->fun->config(pm, p->scl_port, p->scl_pin, &cfg);
        cfg.af = p->sda_af; pm->fun->config(pm, p->sda_port, p->sda_pin, &cfg);
    }
    i2c_hal_enable_clock(p->hal);
    i2c_hal_software_reset(p->hal);
    i2c_hal_config(p->hal, p->clk_hz, p->speed_hz);
    return 0;
}
static int i2c_dev_close(device *self) { i2c *p = (i2c *)self; i2c_hal_set_peripheral_enable(p->hal, 0); pinmux *pm = (pinmux *)device_manager_get("pinmux"); if (pm) pm->fun->release_owner(pm, p->parent.parent.name); return 0; }
static int i2c_dev_read(device *self, void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }
static int i2c_dev_write(device *self, const void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }

static int i2c_control_command(control_device *self, int cmd, void *arg)
{
    i2c *p = (i2c *)self;
    switch (cmd) {
    case I2C_IOCTL_MASTER_WRITE: { if (!arg) return -1; i2c_xfer_t *x = arg; x->result = i2c_hal_master_write(p->hal, x->addr, x->buf, x->len); return x->result; }
    case I2C_IOCTL_MASTER_READ:  { if (!arg) return -1; i2c_xfer_t *x = arg; x->result = i2c_hal_master_read(p->hal, x->addr, x->buf, x->len); return x->result; }
    case I2C_IOCTL_BUS_SCAN: { if (!arg) return -1; i2c_scan_t *s = arg; s->found = 0; memset(s->acks, 0, 128);
        for (int a = 0; a < 128; a++) { if (i2c_hal_master_write(p->hal, (uint16_t)a, NULL, 0) == 0) { s->acks[a] = 1; s->found++; } } return 0; }
    case I2C_IOCTL_SET_SPEED: { if (!arg) return -1; p->speed_hz = *(uint32_t *)arg; i2c_hal_config(p->hal, p->clk_hz, p->speed_hz); return 0; }
    case I2C_IOCTL_GET_CCR:   if (arg) *(uint32_t *)arg = i2c_hal_get_ccr(p->hal); return 0;
    case I2C_IOCTL_GET_CR2_FREQ: if (arg) *(uint32_t *)arg = i2c_hal_get_cr2_freq(p->hal); return 0;
    case I2C_IOCTL_GET_CR1:   if (arg) *(uint32_t *)arg = i2c_hal_get_cr1(p->hal); return 0;
    case I2C_IOCTL_GET_BUSY:  if (arg) *(int *)arg = i2c_hal_is_busy(p->hal); return 0;
    default: return -1;
    }
}
static int i2c_control_set(control_device *self, int p, const void *v) { (void)self;(void)p;(void)v; return -1; }
static int i2c_control_get(control_device *self, int p, void *v) { (void)self;(void)p;(void)v; return -1; }
static int i2c_dev_ioctl(device *self, int cmd, void *arg) { return i2c_control_command((control_device *)self, cmd, arg); }
