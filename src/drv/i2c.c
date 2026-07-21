#include "i2c.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include "irq.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * I2C driver — POLL-only. IRQ support functions and ISRs are registered
 * but NVIC is never enabled (doing so via NVIC_EnableIRQ from this .c file
 * causes a boot hang on this platform). When the NVIC issue is resolved,
 * uncomment NVIC_EnableIRQ in i2c_set_irq() to activate IRQ mode.
 */

/* Peripheral interrupt enable constants for ISR use */
#define I2CSR_SB   (1U << 0)
#define I2CSR_ADDR (1U << 1)
#define I2CSR_BTF  (1U << 2)
#define I2CSR_RXNE (1U << 6)
#define I2CSR_TXE  (1U << 7)
#define I2CSR_AF   (1U << 10)

#define I2C_STATE_IDLE 0
#define I2C_STATE_SB   1
#define I2C_STATE_ADDR 2
#define I2C_STATE_DATA 3
#define I2C_STATE_BTF  4
#define I2C_STATE_DONE 5

static int i2c_dev_open(device *self);
static int i2c_dev_close(device *self);
static int i2c_dev_read(device *self, void *buf, size_t len);
static int i2c_dev_write(device *self, const void *buf, size_t len);
static int i2c_dev_ioctl(device *self, int cmd, void *arg);
static int i2c_control_command(control_device *self, int cmd, void *arg);
static int i2c_control_set(control_device *self, int p, const void *v);
static int i2c_control_get(control_device *self, int p, void *v);
static int  i2c_do_write(i2c *p, uint16_t addr, const uint8_t *buf, uint16_t len);
static int  i2c_do_read(i2c *p, uint16_t addr, uint8_t *buf, uint16_t len);
static void i2c_ev_isr(void *ctx);
static void i2c_er_isr(void *ctx);
static void i2c_set_irq(i2c *p, int on);

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
    i2c *p = (i2c *)malloc(sizeof(i2c)); if (!p) return NULL;
    memset(p, 0, sizeof(i2c));
    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->scl_signal, &sp, &spn, &saf)) { free(p); return NULL; }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->sda_signal, &dp, &dpn, &daf)) { free(p); return NULL; }
    p->hal = i2c_hal_create(c->peripheral); if (!p->hal) { free(p); return NULL; }
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
    if (p->ev_irq >= 0) irq_register(p->ev_irq, i2c_ev_isr, p);
    if (p->er_irq >= 0) irq_register(p->er_irq, i2c_er_isr, p);
    return 0;
}
static int i2c_dev_close(device *self) { i2c *p = (i2c *)self; i2c_set_irq(p, 0); i2c_hal_set_peripheral_enable(p->hal, 0); pinmux *pm = (pinmux *)device_manager_get("pinmux"); if (pm) pm->fun->release_owner(pm, p->parent.parent.name); return 0; }
static int i2c_dev_read(device *self, void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }
static int i2c_dev_write(device *self, const void *buf, size_t len) { (void)self;(void)buf;(void)len; return -1; }

/* POLL-only transfer */
static int i2c_do_write(i2c *p, uint16_t addr, const uint8_t *buf, uint16_t len)
    { (void)p; return i2c_hal_master_write(p->hal, addr, buf, len); }
static int i2c_do_read(i2c *p, uint16_t addr, uint8_t *buf, uint16_t len)
    { (void)p; return i2c_hal_master_read(p->hal, addr, buf, len); }

/* ISRs and IRQ mode switch (active only when NVIC is also enabled) */
static void i2c_set_irq(i2c *p, int on)
{
    if (on && !p->irq_mode) {
        (void)i2c_hal_read_sr1(p->hal);
        i2c_hal_enable_ev_irq(p->hal);
        i2c_hal_enable_er_irq(p->hal);
        p->irq_mode = 1;
    } else if (!on && p->irq_mode) {
        i2c_hal_disable_ev_irq(p->hal);
        i2c_hal_disable_er_irq(p->hal);
        p->irq_mode = 0;
    }
}
static void i2c_ev_isr(void *ctx)
{
    i2c *p = (i2c *)ctx; uint32_t s = i2c_hal_read_sr1(p->hal); int w = (p->tx_buf != NULL);
    if ((s & I2CSR_SB) && p->irq_state == I2C_STATE_SB) { i2c_hal_write_dr(p->hal, (uint8_t)((p->addr << 1) | (w ? 0U : 1U))); p->irq_state = I2C_STATE_ADDR; return; }
    if ((s & I2CSR_ADDR) && p->irq_state == I2C_STATE_ADDR) {
        if (!w) { uint16_t l = p->xfer_len; if (l == 0) { (void)i2c_hal_read_sr2(p->hal); i2c_hal_set_stop(p->hal); p->irq_result = 0; p->irq_state = I2C_STATE_DONE; return; } if (l == 1) { i2c_hal_set_ack(p->hal, 0); i2c_hal_set_stop(p->hal); } else if (l == 2) i2c_hal_set_pos(p->hal, 1); }
        (void)i2c_hal_read_sr2(p->hal); p->xfer_pos = 0; p->irq_state = I2C_STATE_DATA; return;
    }
    if ((s & I2CSR_TXE) && w && p->irq_state == I2C_STATE_DATA) { uint16_t pos = p->xfer_pos; if (pos < p->xfer_len) { i2c_hal_write_dr(p->hal, p->tx_buf[pos]); p->xfer_pos = pos + 1; } if (pos + 1 >= p->xfer_len) p->irq_state = I2C_STATE_BTF; return; }
    if ((s & I2CSR_BTF) && p->irq_state == I2C_STATE_BTF) {
        if (!w) { if (p->xfer_len == 2) { uint8_t b0 = i2c_hal_read_dr(p->hal), b1 = i2c_hal_read_dr(p->hal); if (p->rx_buf) { p->rx_buf[0] = b0; p->rx_buf[1] = b1; } i2c_hal_set_stop(p->hal); i2c_hal_set_pos(p->hal, 0); p->irq_result = 0; p->irq_state = I2C_STATE_DONE; return; }
            uint16_t pos = p->xfer_pos; if (pos < p->xfer_len && p->rx_buf) p->rx_buf[pos] = i2c_hal_read_dr(p->hal); p->xfer_pos = pos + 1; p->irq_state = I2C_STATE_DATA; return; }
        i2c_hal_set_stop(p->hal); p->irq_result = 0; p->irq_state = I2C_STATE_DONE; return;
    }
    if ((s & I2CSR_RXNE) && !w && p->irq_state == I2C_STATE_DATA) {
        uint16_t pos = p->xfer_pos; uint8_t d = i2c_hal_read_dr(p->hal); if (pos < p->xfer_len && p->rx_buf) p->rx_buf[pos] = d; pos++; p->xfer_pos = pos;
        if (p->xfer_len == 1) { p->irq_result = 0; p->irq_state = I2C_STATE_DONE; return; }
        if (p->xfer_len >= 3 && pos >= p->xfer_len - 1) { p->irq_result = 0; p->irq_state = I2C_STATE_DONE; return; }
        if (p->xfer_len >= 3 && pos == p->xfer_len - 2) { i2c_hal_set_ack(p->hal, 0); p->irq_state = I2C_STATE_BTF; }
    }
}
static void i2c_er_isr(void *ctx) { i2c *p = (i2c *)ctx; if (i2c_hal_read_sr1(p->hal) & I2CSR_AF) { (void)i2c_hal_read_sr1(p->hal); i2c_hal_set_stop(p->hal); p->irq_result = -1; p->irq_state = I2C_STATE_DONE; } }

static int i2c_control_command(control_device *self, int cmd, void *arg)
{
    i2c *p = (i2c *)self;
    switch (cmd) {
    case I2C_IOCTL_MASTER_WRITE: { i2c_xfer_t *x = arg; if (!x) return -1; x->result = i2c_do_write(p, x->addr, x->buf, x->len); return x->result; }
    case I2C_IOCTL_MASTER_READ:  { i2c_xfer_t *x = arg; if (!x) return -1; x->result = i2c_do_read(p, x->addr, x->buf, x->len); return x->result; }
    case I2C_IOCTL_BUS_SCAN: { i2c_scan_t *s = arg; if (!s) return -1; s->found = 0; memset(s->acks, 0, 128); for (int a = 0; a < 128; a++) { if (i2c_do_write(p, (uint16_t)a, NULL, 0) == 0) { s->acks[a] = 1; s->found++; } } return 0; }
    case I2C_IOCTL_SET_SPEED: { if (!arg) return -1; p->speed_hz = *(uint32_t *)arg; i2c_hal_config(p->hal, p->clk_hz, p->speed_hz); return 0; }
    case I2C_IOCTL_GET_CCR:   if (arg) *(uint32_t *)arg = i2c_hal_get_ccr(p->hal); return 0;
    case I2C_IOCTL_GET_CR2_FREQ: if (arg) *(uint32_t *)arg = i2c_hal_get_cr2_freq(p->hal); return 0;
    case I2C_IOCTL_GET_CR1:   if (arg) *(uint32_t *)arg = i2c_hal_get_cr1(p->hal); return 0;
    case I2C_IOCTL_GET_BUSY:  if (arg) *(int *)arg = i2c_hal_is_busy(p->hal); return 0;
    case I2C_IOCTL_SET_IRQ_MODE: { if (!arg) return -1; i2c_set_irq(p, *(int *)arg); return 0; }
    default: return -1;
    }
}
static int i2c_control_set(control_device *self, int p, const void *v) { (void)self;(void)p;(void)v; return -1; }
static int i2c_control_get(control_device *self, int p, void *v) { (void)self;(void)p;(void)v; return -1; }
static int i2c_dev_ioctl(device *self, int cmd, void *arg) { return i2c_control_command((control_device *)self, cmd, arg); }
