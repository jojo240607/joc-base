#include "can.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* virtual implementations dispatched through the unified device vtable */
static int can_dev_open(device *self);
static int can_dev_close(device *self);
static int can_dev_read(device *self, void *buf, size_t len);
static int can_dev_write(device *self, const void *buf, size_t len);
static int can_dev_ioctl(device *self, int cmd, void *arg);

/* stream-class vtable (read/write/flush/frame) */
static int can_stream_read(stream_device *self, void *buf, size_t len);
static int can_stream_write(stream_device *self, const void *buf, size_t len);
static int can_stream_flush(stream_device *self);
static int can_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta);
static int can_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta);

static const struct stream_deviceVtable can_stream_vtable = {
    .read        = can_stream_read,
    .write       = can_stream_write,
    .flush       = can_stream_flush,
    .read_frame  = can_stream_read_frame,
    .write_frame = can_stream_write_frame,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable can_dev_vtable = {
    .open  = can_dev_open,
    .close = can_dev_close,
    .read  = can_dev_read,
    .write = can_dev_write,
    .ioctl = can_dev_ioctl,
};

device *can_create(const void *config)
{
    const can_config_t *c = (const can_config_t *)config;
    if (!c || !c->tx_signal || !c->rx_signal)
        return NULL;

    can *p = (can *)malloc(sizeof(can));
    if (!p) return NULL;
    memset(p, 0, sizeof(can));

    pinmux_port_t txp; uint8_t txpn, txaf;
    if (!pinmux_hal_resolve(c->tx_signal, &txp, &txpn, &txaf)) {
        printf("[can] %s: unknown TX \"%s\"\r\n", c->name, c->tx_signal);
        free(p); return NULL;
    }
    pinmux_port_t rxp; uint8_t rxpn, rxaf;
    if (!pinmux_hal_resolve(c->rx_signal, &rxp, &rxpn, &rxaf)) {
        printf("[can] %s: unknown RX \"%s\"\r\n", c->name, c->rx_signal);
        free(p); return NULL;
    }

    p->hal = can_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &can_dev_vtable;
    p->parent.vtable        = &can_stream_vtable;
    p->parent.parent.type   = DEVICE_TYPE_CAN;
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.parent.name   = c->name;
    p->parent.mode          = STREAM_MODE_POLL;   /* only POLL supported */
    p->periph   = c->periph;
    p->pclk_hz  = c->pclk_hz;
    p->presc    = c->presc ? c->presc : 4U;
    p->sjw      = c->sjw   ? c->sjw   : 1U;
    p->bs1      = c->bs1   ? c->bs1   : 11U;
    p->bs2      = c->bs2   ? c->bs2   : 4U;
    p->loopback = c->loopback ? 1 : 0;
    p->silent   = c->silent   ? 1 : 0;
    p->remap    = c->remap ? 1 : 0;
    p->tx_id    = c->tx_id ? c->tx_id : 0x123U;
    p->tx_port = txp; p->tx_pin = txpn; p->tx_af = txaf;
    p->rx_port = rxp; p->rx_pin = rxpn; p->rx_af = rxaf;

    return &p->parent.parent;
}

void can_destroy(can *self)
{
    if (!self) return;
    can_hal_destroy(self->hal);
    free(self);
}

static int can_dev_open(device *self)
{
    can *p = (can *)self;

    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->tx_port, p->tx_pin, p->tx_af, p->parent.parent.name) != 0) {
            printf("[can] %s: TX P%c%d CONFLICT\r\n", p->parent.parent.name,
                   'A' + (int)p->tx_port, (int)p->tx_pin); return -2;
        }
        if (pm->fun->request(pm, p->rx_port, p->rx_pin, p->rx_af, p->parent.parent.name) != 0) {
            printf("[can] %s: RX P%c%d CONFLICT\r\n", p->parent.parent.name,
                   'A' + (int)p->rx_port, (int)p->rx_pin); return -2;
        }
        pinmux_pin_cfg_t cfg = { .af = p->tx_af, .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };
        pm->fun->config(pm, p->tx_port, p->tx_pin, &cfg);
        /* RX pin needs an internal PULL-UP: with no CAN transceiver on the board
         * PA11 is floating, so the bus-idle detector (11 recessive bits) needed
         * to leave init mode would never see a recessive level. Pulling it up
         * makes the (loopback) bus idle detectable. */
        cfg.af = p->rx_af; cfg.pupd = 1;
        pm->fun->config(pm, p->rx_port, p->rx_pin, &cfg);
    }

    can_hal_enable_clock(p->hal);
    int rc = can_hal_init(p->hal, p->pclk_hz, p->presc, p->sjw, p->bs1, p->bs2,
                          p->loopback, p->silent, p->remap);
    return rc;
}

static int can_dev_close(device *self)
{
    can *p = (can *)self;
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* =========================================================================
 * Stream-class virtual implementations
 * ========================================================================= */

/* Transmit `len` bytes (<= 8) as a single frame using the default TX id.
 * Returns the number of bytes written, or -1 on error. */
static int can_stream_write(stream_device *self, const void *buf, size_t len)
{
    can *p = (can *)self;
    if (len > 8U) len = 8U;
    if (len == 0) return 0;

    can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id  = p->tx_id;
    f.dlc = (uint8_t)len;
    memcpy(f.data, buf, len);
    if (can_hal_send(p->hal, &f, 0) != 0) return -1;
    return (int)len;
}

/* Receive one frame; copy its data bytes into buf (up to `len`).
 * Returns the number of bytes received (the frame DLC), or -1 on error. */
static int can_stream_read(stream_device *self, void *buf, size_t len)
{
    can *p = (can *)self;
    can_frame_t f;
    if (can_hal_recv(p->hal, &f, 0) != 0) return -1;
    size_t n = f.dlc;
    if (n > len) n = len;
    memcpy(buf, f.data, n);
    return (int)n;
}

static int can_stream_flush(stream_device *self) { (void)self; return 0; }

static int can_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
{
    can *p = (can *)self;
    if (len > 8U) len = 8U;
    if (len == 0) return -1;

    can_frame_t f;
    memset(&f, 0, sizeof(f));
    if (meta) f.id = *(const uint32_t *)meta;   /* stream meta carries the CAN id */
    else      f.id = p->tx_id;
    f.dlc = (uint8_t)len;
    memcpy(f.data, buf, len);
    if (can_hal_send(p->hal, &f, 0) != 0) return -1;
    return (int)len;
}

static int can_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
{
    can *p = (can *)self;
    can_frame_t f;
    if (can_hal_recv(p->hal, &f, 0) != 0) return -1;
    size_t n = f.dlc;
    if (n > len) n = len;
    memcpy(buf, f.data, n);
    if (meta) *(uint32_t *)meta = f.id;          /* return the received id in meta */
    return (int)n;
}

/* =========================================================================
 * Base device vtable: forward to stream vtable, plus ioctl
 * ========================================================================= */
static int can_dev_read(device *self, void *buf, size_t len)
    { return can_stream_read((stream_device *)self, buf, len); }

static int can_dev_ioctl(device *self, int cmd, void *arg)
{
    can *p = (can *)self;
    switch (cmd) {
    case CAN_IOCTL_SEND_FRAME:
        if (!arg) return -1;
        return can_hal_send(p->hal, (const can_frame_t *)arg, 0);
    case CAN_IOCTL_RECV_FRAME:
        if (!arg) return -1;
        return can_hal_recv(p->hal, (can_frame_t *)arg, 0);
    case CAN_IOCTL_GET_MCR:  if (arg) *(uint32_t *)arg = can_hal_get_mcr(p->hal);  return 0;
    case CAN_IOCTL_GET_BTR:  if (arg) *(uint32_t *)arg = can_hal_get_btr(p->hal);  return 0;
    case CAN_IOCTL_GET_MSR:  if (arg) *(uint32_t *)arg = can_hal_get_msr(p->hal);  return 0;
    case CAN_IOCTL_GET_ESR:  if (arg) *(uint32_t *)arg = can_hal_get_esr(p->hal);  return 0;
    case CAN_IOCTL_GET_TSR:  if (arg) *(uint32_t *)arg = can_hal_get_tsr(p->hal);  return 0;
    case CAN_IOCTL_GET_RF0R: if (arg) *(uint32_t *)arg = can_hal_get_rf0r(p->hal); return 0;
    case CAN_IOCTL_GET_FA1R: if (arg) *(uint32_t *)arg = can_hal_get_fa1r(p->hal); return 0;
    case CAN_IOCTL_GET_FMR:  if (arg) *(uint32_t *)arg = can_hal_get_fmr(p->hal);  return 0;
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

static int can_dev_write(device *self, const void *buf, size_t len)
    { return can_stream_write((stream_device *)self, buf, len); }
