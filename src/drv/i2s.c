#include "i2s.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/* Fixed PLLI2S settings used by the driver (PLLI2S input = HSE/PLLM = 1 MHz).
 * N=258, R=3 => PLLI2SVCO = 258 MHz, I2SxCLK = 258/3 = 86 MHz. The prescaler in
 * i2s_hal_config() then divides that to the requested audio sample rate. */
#define I2S_PLLI2SN  258U
#define I2S_PLLI2SR  3U

/* virtual implementations dispatched through the unified device vtable */
static int i2s_dev_open(device *self);
static int i2s_dev_close(device *self);
static int i2s_dev_read(device *self, void *buf, size_t len);
static int i2s_dev_write(device *self, const void *buf, size_t len);
static int i2s_dev_ioctl(device *self, int cmd, void *arg);

/* stream-class vtable (read/write/flush) */
static int i2s_stream_read(stream_device *self, void *buf, size_t len);
static int i2s_stream_write(stream_device *self, const void *buf, size_t len);
static int i2s_stream_flush(stream_device *self);
static int i2s_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta);
static int i2s_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta);

static const struct stream_deviceVtable i2s_stream_vtable = {
    .read        = i2s_stream_read,
    .write       = i2s_stream_write,
    .flush       = i2s_stream_flush,
    .read_frame  = i2s_stream_read_frame,
    .write_frame = i2s_stream_write_frame,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable i2s_dev_vtable = {
    .open  = i2s_dev_open,
    .close = i2s_dev_close,
    .read  = i2s_dev_read,
    .write = i2s_dev_write,
    .ioctl = i2s_dev_ioctl,
};

device *i2s_create(const void *config)
{
    const i2s_config_t *c = (const i2s_config_t *)config;
    if (!c || !c->ws_signal || !c->ck_signal || !c->sd_signal)
        return NULL;

    i2s *p = (i2s *)malloc(sizeof(i2s));
    if (!p) return NULL;
    memset(p, 0, sizeof(i2s));

    pinmux_port_t wp; uint8_t wpn, waf;
    if (!pinmux_hal_resolve(c->ws_signal, &wp, &wpn, &waf)) {
        log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: unknown WS \"%s\"\n", c->name, c->ws_signal);
        free(p); return NULL;
    }
    pinmux_port_t kp; uint8_t kpn, kaf;
    if (!pinmux_hal_resolve(c->ck_signal, &kp, &kpn, &kaf)) {
        log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: unknown CK \"%s\"\n", c->name, c->ck_signal);
        free(p); return NULL;
    }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->sd_signal, &dp, &dpn, &daf)) {
        log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: unknown SD \"%s\"\n", c->name, c->sd_signal);
        free(p); return NULL;
    }
    if (waf != kaf || waf != daf) {
        log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: AF mismatch WS=%u CK=%u SD=%u\n",
               c->name, (unsigned)waf, (unsigned)kaf, (unsigned)daf);
        free(p); return NULL;
    }

    p->has_esd = 0;
    if (c->extsd_signal) {
        if (pinmux_hal_resolve(c->extsd_signal, &p->esd_port, &p->esd_pin, &p->esd_af)) {
            p->has_esd = 1;
        } else {
            log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: unknown extSD \"%s\" (ignored)\n",
                   c->name, c->extsd_signal);
        }
    }

    p->hal = i2s_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &i2s_dev_vtable;
    p->parent.vtable        = &i2s_stream_vtable;
    p->parent.parent.type   = DEVICE_TYPE_I2S;
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.parent.name   = c->name;
    p->parent.mode          = STREAM_MODE_POLL;   /* only POLL supported */
    p->periph   = c->periph;
    p->pclk_hz  = c->pclk_hz;
    p->audio_hz = c->audio_hz;
    p->master   = c->master;
    p->tx       = c->tx;
    p->datlen   = c->datlen;
    p->ws_port = wp; p->ws_pin = wpn; p->ws_af = waf;
    p->ck_port = kp; p->ck_pin = kpn; p->ck_af = kaf;
    p->sd_port = dp; p->sd_pin = dpn; p->sd_af = daf;

    return &p->parent.parent;
}

void i2s_destroy(i2s *self)
{
    if (!self) return;
    i2s_hal_destroy(self->hal);
    free(self);
}

static int i2s_dev_open(device *self)
{
    i2s *p = (i2s *)self;

    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->ws_port, p->ws_pin, p->ws_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: WS P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->ws_port, (int)p->ws_pin); return -2;
        }
        if (pm->fun->request(pm, p->ck_port, p->ck_pin, p->ck_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: CK P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->ck_port, (int)p->ck_pin); return -2;
        }
        if (pm->fun->request(pm, p->sd_port, p->sd_pin, p->sd_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: SD P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->sd_port, (int)p->sd_pin); return -2;
        }
        if (p->has_esd) {
            if (pm->fun->request(pm, p->esd_port, p->esd_pin, p->esd_af, p->parent.parent.name) != 0) {
                log_printf(app_log(), LOG_DEBUG, "i2s", "[i2s] %s: extSD P%c%d CONFLICT\n", p->parent.parent.name,
                       'A' + (int)p->esd_port, (int)p->esd_pin); return -2;
            }
        }
        pinmux_pin_cfg_t cfg = { .af = p->ws_af, .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };
        pm->fun->config(pm, p->ws_port, p->ws_pin, &cfg);
        cfg.af = p->ck_af;
        pm->fun->config(pm, p->ck_port, p->ck_pin, &cfg);
        cfg.af = p->sd_af;
        pm->fun->config(pm, p->sd_port, p->sd_pin, &cfg);
        if (p->has_esd) {
            cfg.af = p->esd_af; cfg.pupd = 1;
            pm->fun->config(pm, p->esd_port, p->esd_pin, &cfg);
        }
    }

    /* Bring up the dedicated audio clock first — TXE will never assert without it.
     * config_pll returns the resulting PLLI2S output (I2SxCLK) which the prescaler
     * in i2s_hal_config() needs to divide down to the requested sample rate. */
    i2s_hal_enable_clock(p->hal);
    uint32_t i2s_clk = i2s_hal_config_pll(p->hal, I2S_PLLI2SN, I2S_PLLI2SR);

    i2s_hal_config(p->hal, 0 /*Philips*/, p->datlen, p->audio_hz,
                   i2s_clk, p->master, p->tx);
    i2s_hal_enable(p->hal, 1);
    return 0;
}

static int i2s_dev_close(device *self)
{
    i2s *p = (i2s *)self;
    i2s_hal_enable(p->hal, 0);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* =========================================================================
 * Stream-class virtual implementations
 * ========================================================================= */

/* Transmit `len` bytes as 16-bit samples (the driver is configured for 16-bit
 * data). Returns the number of bytes written, or -1 on error. */
static int i2s_stream_write(stream_device *self, const void *buf, size_t len)
{
    i2s *p = (i2s *)self;
    if (!p->tx) return -1;                       /* RX-only instance */
    if (len < 2) return 0;
    const uint16_t *s = (const uint16_t *)buf;
    size_t n = len / 2;
    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (i2s_hal_write_sample(p->hal, s[i]) != 0) { ok = 0; break; }
    }
    return ok ? (int)(n * 2) : -1;
}

static int i2s_stream_read(stream_device *self, void *buf, size_t len)
{
    (void)buf; (void)len;
    /* The BIST instance is master TX; reading back transmitted data is undefined
     * (I2S DR read returns the RECEIVE slot). RX instances would poll RXNE here. */
    return -1;
}

static int i2s_stream_flush(stream_device *self) { (void)self; return 0; }
static int i2s_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }
static int i2s_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }

/* =========================================================================
 * Base device vtable: forward to stream vtable, plus ioctl
 * ========================================================================= */
static int i2s_dev_read(device *self, void *buf, size_t len)
    { return i2s_stream_read((stream_device *)self, buf, len); }
static int i2s_dev_write(device *self, const void *buf, size_t len)
    { return i2s_stream_write((stream_device *)self, buf, len); }

static int i2s_dev_ioctl(device *self, int cmd, void *arg)
{
    i2s *p = (i2s *)self;
    switch (cmd) {
    case I2S_IOCTL_GET_I2SCFGR:
        if (arg) *(uint32_t *)arg = i2s_hal_get_i2scfgr(p->hal);
        return 0;
    case I2S_IOCTL_GET_I2SPR:
        if (arg) *(uint32_t *)arg = i2s_hal_get_i2spr(p->hal);
        return 0;
    case I2S_IOCTL_GET_PLLI2S:
        if (arg) *(uint32_t *)arg = i2s_hal_get_plli2s();
        return 0;
    case I2S_IOCTL_GET_CFGR:
        if (arg) *(uint32_t *)arg = i2s_hal_get_cfgr();
        return 0;
    case I2S_IOCTL_GET_PLL_RDY:
        if (arg) *(uint32_t *)arg = i2s_hal_pll_rdy();
        return 0;
    case I2S_IOCTL_GET_AUDIO_HZ:
        if (arg) *(uint32_t *)arg = p->audio_hz;
        return 0;
    case I2S_IOCTL_GET_I2S_CLK:
        if (arg) *(uint32_t *)arg = i2s_hal_get_i2s_clk_hz(p->hal);
        return 0;
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        if (m != STREAM_MODE_POLL) return -1;   /* only POLL supported */
        p->parent.mode = m;
        return 0;
    }
    case STREAM_IOCTL_GET_MODE:
        if (arg) *(stream_xfer_mode_t *)arg = p->parent.mode;
        return 0;
    default:
        return -1;
    }
}
