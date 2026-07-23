#include "usb.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include "hal/stm32/usb_hal.h"
#include "irq_manager.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- CDC descriptors -------------------------------------------------- */

/* Device descriptor (18 bytes). Miscellaneous class + IAD so Windows loads the
 * native usbser driver without an INF. */
static const uint8_t dev_desc[18] = {
    0x12,             /* bLength */
    0x01,             /* bDescriptorType = Device */
    0x00, 0x02,       /* bcdUSB = 2.00 */
    0xEF,             /* bDeviceClass = Miscellaneous */
    0x02,             /* bDeviceSubClass = Common Class */
    0x01,             /* bDeviceProtocol = IAD */
    0x40,             /* bMaxPacketSize0 = 64 */
    0x83, 0x04,       /* idVendor = 0x0483 (ST) */
    0x40, 0x57,       /* idProduct = 0x5740 (STM CDC) */
    0x00, 0x02,       /* bcdDevice = 2.00 */
    0x01,             /* iManufacturer */
    0x02,             /* iProduct */
    0x03,             /* iSerialNumber */
    0x01              /* bNumConfigurations = 1 */
};
#define DEV_DESC_LEN 18

/* Full configuration descriptor: config header + IAD + Communication IF
 * (Header/ACM/Union/CallMgmt + notification EP2 IN) + Data IF (bulk EP1 OUT/IN).
 * Total length = 75 (9+8+9+5+4+5+5+7+9+7+7). */
static const uint8_t cfg_desc[75] = {
    /* Configuration descriptor (9) */
    0x09, 0x02, 0x4B, 0x00, 0x02, 0x01, 0x00, 0xC0, 0x32,
    /* Interface Association Descriptor (8) */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x00, 0x00,
    /* Communication Interface (IF0) (9) */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00,
    /* CDC Header Functional (5) */
    0x05, 0x24, 0x00, 0x10, 0x01,
    /* CDC ACM Functional (4) */
    0x04, 0x24, 0x02, 0x02,
    /* CDC Union Functional (5) */
    0x05, 0x24, 0x06, 0x00, 0x01,
    /* CDC Call Management Functional (5) */
    0x05, 0x24, 0x01, 0x00, 0x01,
    /* Notification EP (EP2 IN, interrupt) (7) */
    0x07, 0x05, 0x82, 0x03, 0x0A, 0x00, 0x10,
    /* Data Interface (IF1) (9) */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    /* Bulk OUT (EP1 OUT) (7) */
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
    /* Bulk IN (EP1 IN) (7) */
    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00
};
#define CFG_DESC_LEN 75

/* ---- driver object ---------------------------------------------------- */

struct _usb {
    stream_device parent;          /* IS-A stream_device IS-A device */
    usb_hal_handle_t *hal;
    const char *dm_signal;
    const char *dp_signal;

    uint8_t  address;
    uint8_t  config;
    int      connected;
    uint8_t  pending_address;      /* latched by SET_ADDRESS, applied on status */

    /* EP0 control transfer state */
    uint8_t  setup[8];
    const uint8_t *ep0_in_src;
    uint16_t ep0_in_total, ep0_in_rem, ep0_in_off;
    uint8_t  ctrl_out_kind;        /* 0 none, 1 = SET_LINE_CODING pending */
    uint16_t ctrl_out_len;
    uint8_t  ctrl_buf[64];

    /* CDC line coding / state (7 bytes: 4B baud LE, 1B stop, 1B parity, 1B data) */
    uint8_t  line_coding[7];
    uint8_t  line_state;           /* DTR/RTS from SET_CONTROL_LINE_STATE */

    int      bulk_tx_pending;

    uint8_t  rx_storage[USB_RX_BUF_SIZE];

    /* host-free self-test mode */
    int      test_mode;
    uint8_t  test_ep0_in[128];
    uint16_t test_ep0_in_len;
};

/* ---- vtable forward decls --------------------------------------------- */

static int  usb_dev_open(device *self);
static int  usb_dev_close(device *self);
static int  usb_dev_read(device *self, void *buf, size_t len);
static int  usb_dev_write(device *self, const void *buf, size_t len);
static int  usb_dev_ioctl(device *self, int cmd, void *arg);

static int  usb_stream_read(stream_device *self, void *buf, size_t len);
static int  usb_stream_write(stream_device *self, const void *buf, size_t len);
static int  usb_stream_flush(stream_device *self);
static int  usb_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta);
static int  usb_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta);

static void usb_isr(void *ctx);
static void usb_on_usb_reset(usb *u);

static const struct stream_deviceVtable usb_stream_vtable = {
    .read        = usb_stream_read,
    .write        = usb_stream_write,
    .flush        = usb_stream_flush,
    .read_frame   = usb_stream_read_frame,
    .write_frame  = usb_stream_write_frame,
    .submit       = NULL,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable usb_dev_vtable = {
    .open  = usb_dev_open,
    .close = usb_dev_close,
    .read  = usb_dev_read,
    .write = usb_dev_write,
    .ioctl = usb_dev_ioctl,
};

/* ---- create / destroy ------------------------------------------------ */

device *usb_create(const void *config)
{
    const usb_config_t *c = (const usb_config_t *)config;
    if (!c || !c->dm_signal || !c->dp_signal)
        return NULL;

    usb *p = (usb *)malloc(sizeof(usb));
    if (!p) return NULL;
    memset(p, 0, sizeof(usb));

    p->hal = usb_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &usb_dev_vtable;
    p->parent.vtable        = &usb_stream_vtable;
    p->parent.parent.type   = DEVICE_TYPE_USB;
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.parent.name   = c->name;
    p->parent.mode          = STREAM_MODE_IRQ;
    p->dm_signal = c->dm_signal;
    p->dp_signal = c->dp_signal;
    /* default line coding: 115200 8N1 (little-endian 32-bit baud = 0x0001C200) */
    p->line_coding[0] = 0x00; p->line_coding[1] = 0xC2;
    p->line_coding[2] = 0x01; p->line_coding[3] = 0x00;
    p->line_coding[4] = 0x00;   /* 1 stop bit */
    p->line_coding[5] = 0x00;   /* no parity */
    p->line_coding[6] = 0x08;   /* 8 data bits */
    return (device *)p;
}

void usb_destroy(usb *self)
{
    if (!self) return;
    stream_device_free_ringbuffer((stream_device *)self);
    usb_hal_destroy(self->hal);
    free(self);
}

/* ---- control transfer engine ----------------------------------------- */

/* Queue EP0 IN data (possibly multi-packet). In test mode, copy the whole
 * buffer into test_ep0_in for inspection instead of touching the FIFO. */
static void usb_ctrl_send(usb *u, const uint8_t *buf, uint16_t len)
{
    if (u->test_mode) {
        memcpy(u->test_ep0_in, buf, len);
        u->test_ep0_in_len = len;
        return;
    }
    u->ep0_in_src   = buf;
    u->ep0_in_total = len;
    u->ep0_in_rem   = len;
    u->ep0_in_off   = 0;
    uint16_t chunk = len < 64 ? len : 64;
    usb_hal_ep_tx(u->hal, 0, buf, chunk);
    u->ep0_in_rem -= chunk;
    u->ep0_in_off += chunk;
    if (u->ep0_in_rem == 0)
        usb_hal_ep_rx(u->hal, 0, 64);   /* host will send the status OUT ZLP */
}

/* EP0 IN transfer complete (XFRC). Continue a multi-packet send, terminate a
 * 64-byte-multiple data stage with a ZLP, or finish a status stage (apply the
 * latched address for SET_ADDRESS). */
static void usb_ep0_in_complete(usb *u)
{
    if (u->ep0_in_rem > 0) {
        uint16_t chunk = u->ep0_in_rem < 64 ? u->ep0_in_rem : 64;
        usb_hal_ep_tx(u->hal, 0, u->ep0_in_src + u->ep0_in_off, chunk);
        u->ep0_in_off += chunk;
        u->ep0_in_rem -= chunk;
        if (u->ep0_in_rem == 0) {
            if (u->ep0_in_total > 0 && (u->ep0_in_total % 64) == 0)
                usb_hal_ep0_tx_zlp(u->hal);   /* terminate data stage */
            usb_hal_ep_rx(u->hal, 0, 64);     /* arm status OUT */
        }
        return;
    }
    /* status ZLP IN completion (OUT-type request with no data stage) */
    if (u->pending_address) {
        usb_hal_set_address(u->hal, u->pending_address);
        u->pending_address = 0;
    }
    usb_hal_ep_rx(u->hal, 0, 64);   /* ready for the next SETUP */
}

/* OUT data stage landed on EP0 (e.g. SET_LINE_CODING). */
static void usb_handle_ep0_out_data(usb *u, uint16_t bcnt)
{
    if (u->ctrl_out_kind == 1 && bcnt == 7) {
        memcpy(u->line_coding, u->ctrl_buf, 7);
        u->ctrl_out_kind = 0;
        usb_hal_ep0_tx_zlp(u->hal);   /* status IN */
    }
    /* else: status-stage ZLP from host (bcnt==0) — nothing to do */
}

static void usb_get_string(usb *u, uint8_t index, uint16_t wLength)
{
    static const char *strs[3] = { NULL, "joc-base", "CDC-ACM" };
    uint8_t buf[64];
    uint16_t n = 0;
    if (index == 0) {
        buf[0] = 4; buf[1] = 0x03; buf[2] = 0x09; buf[3] = 0x04; n = 4;
    } else if (index >= 1 && index <= 2) {
        const char *s = strs[index];
        uint8_t slen = (uint8_t)strlen(s);
        buf[0] = (uint8_t)(2 + slen * 2); buf[1] = 0x03;
        for (uint8_t i = 0; i < slen; i++) {
            buf[2 + 2*i] = (uint8_t)s[i];
            buf[3 + 2*i] = 0;
        }
        n = buf[0];
    } else {
        usb_hal_ep0_tx_zlp(u->hal);   /* unsupported string -> stall-ish */
        return;
    }
    uint16_t send = n < wLength ? n : wLength;
    usb_ctrl_send(u, buf, send);
}

static void usb_get_descriptor(usb *u, uint8_t desc_type, uint8_t index, uint16_t wLength)
{
    (void)index;
    if (desc_type == 0x01) {
        uint16_t n = DEV_DESC_LEN < wLength ? DEV_DESC_LEN : wLength;
        usb_ctrl_send(u, dev_desc, n);
    } else if (desc_type == 0x02) {
        uint16_t n = CFG_DESC_LEN < wLength ? CFG_DESC_LEN : wLength;
        usb_ctrl_send(u, cfg_desc, n);
    } else if (desc_type == 0x03) {
        usb_get_string(u, index, wLength);
    } else {
        usb_hal_ep0_tx_zlp(u->hal);
    }
}

/* Parse an 8-byte SETUP and drive the control transfer. */
static void usb_ctrl_dispatch(usb *u)
{
    uint8_t  *s   = u->setup;
    uint8_t  bmReq = s[0], bReq = s[1];
    uint16_t wValue = (uint16_t)s[2] | ((uint16_t)s[3] << 8);
    uint16_t wIndex = (uint16_t)s[4] | ((uint16_t)s[5] << 8);
    uint16_t wLength= (uint16_t)s[6] | ((uint16_t)s[7] << 8);
    uint8_t  type = (bmReq >> 5) & 3;
    uint8_t  recpt= bmReq & 0x1F;
    (void)wIndex;

    if (type == 0) {                          /* standard request */
        switch (bReq) {
        case 0x00: { uint8_t st[2] = {0,0}; usb_ctrl_send(u, st, 2); } break; /* GET_STATUS */
        case 0x05:                            /* SET_ADDRESS */
            u->pending_address = wValue & 0x7F;
            usb_hal_ep0_tx_zlp(u->hal);       /* status IN; address applied on IN done */
            break;
        case 0x06: usb_get_descriptor(u, (uint8_t)(wValue >> 8), (uint8_t)(wValue & 0xFF), wLength); break;
        case 0x08: { uint8_t c[1] = {(uint8_t)(u->config ? 1 : 0)}; usb_ctrl_send(u, c, 1); } break;
        case 0x09:                            /* SET_CONFIGURATION */
            u->config = (uint8_t)wValue;
            u->connected = (wValue != 0);
            usb_hal_ep_rx(u->hal, 1, 64);     /* arm bulk OUT */
            usb_hal_ep0_tx_zlp(u->hal);
            break;
        case 0x01: case 0x03:                 /* CLEAR/SET_FEATURE */
            usb_hal_ep0_tx_zlp(u->hal); break;
        default:
            usb_hal_ep0_tx_zlp(u->hal); break;
        }
    } else if (type == 1 && recpt == 1) {    /* CDC class on an interface */
        switch (bReq) {
        case 0x20:                            /* SET_LINE_CODING (OUT, 7 bytes) */
            u->ctrl_out_kind = 1;
            usb_hal_ep_rx(u->hal, 0, 64);     /* receive the 7 bytes */
            break;
        case 0x21:                            /* GET_LINE_CODING */
            usb_ctrl_send(u, u->line_coding, 7); break;
        case 0x22:                            /* SET_CONTROL_LINE_STATE */
            u->line_state = (uint8_t)(wValue & 0x3);
            usb_hal_ep0_tx_zlp(u->hal); break;
        default:
            usb_hal_ep0_tx_zlp(u->hal); break;
        }
    } else {
        usb_hal_ep0_tx_zlp(u->hal);           /* not supported: ACK with ZLP */
    }
}

/* RxFIFO drain: SETUP packets are dispatched, OUT packets are read into the
 * right buffer (EP0 control data / EP1 bulk into the RX ring). */
static void usb_handle_rx(usb *u)
{
    usb_hal_handle_t *h = u->hal;
    while (usb_hal_gintsts_raw(h) & USB_HAL_GINT_RXFLVL) {
        uint32_t word = usb_hal_rxstsp(h);
        uint8_t  ep   = (uint8_t)(word & 0xFUL);
        uint8_t  pkt  = (uint8_t)((word >> USB_OTG_GRXSTSP_PKTSTS_Pos) & 0xFUL);
        uint16_t bcnt = (uint16_t)((word >> USB_OTG_GRXSTSP_BCNT_Pos) & 0x7FFUL);

        if (pkt == 0x4) {                     /* SETUP */
            usb_hal_fifo_read(h, u->setup, 8);
            usb_ctrl_dispatch(u);
            usb_hal_ep_rx(h, 0, 64);
        } else if (pkt == 0x2) {              /* OUT data */
            if (ep == 0) {
                usb_hal_fifo_read(h, u->ctrl_buf, bcnt);
                usb_handle_ep0_out_data(u, bcnt);
                usb_hal_ep_rx(h, 0, 64);
            } else if (ep == 1) {
                uint8_t tmp[64];
                usb_hal_fifo_read(h, tmp, bcnt);
                ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
                for (uint16_t i = 0; i < bcnt && rb; i++)
                    rb->fun->put(rb, tmp[i]);
                usb_hal_ep_rx(h, 1, 64);
            }
        }
        /* 0x1/0x3/0x6: status completions, no payload */
    }
}

static void usb_handle_oep(usb *u)
{
    usb_hal_handle_t *h = u->hal;
    uint32_t daint = usb_hal_daint(h);
    if (daint & (1UL << 16)) { uint32_t m = usb_hal_doepint(h, 0); usb_hal_doepint_clear(h, 0, m); }
    if (daint & (1UL << 17)) { uint32_t m = usb_hal_doepint(h, 1); usb_hal_doepint_clear(h, 1, m); }
}

static void usb_handle_iep(usb *u)
{
    usb_hal_handle_t *h = u->hal;
    uint32_t daint = usb_hal_daint(h);
    if (daint & (1UL << 0)) {
        uint32_t m = usb_hal_diepint(h, 0);
        if (m & USB_OTG_DIEPINT_XFRC_Msk) usb_ep0_in_complete(u);
        usb_hal_diepint_clear(h, 0, m);
    }
    if (daint & (1UL << 1)) {
        uint32_t m = usb_hal_diepint(h, 1);
        if (m & USB_OTG_DIEPINT_XFRC_Msk) u->bulk_tx_pending = 0;
        usb_hal_diepint_clear(h, 1, m);
    }
    if (daint & (1UL << 2)) {
        uint32_t m = usb_hal_diepint(h, 2);
        usb_hal_diepint_clear(h, 2, m);
    }
}

static void usb_isr(void *ctx)
{
    usb *u = (usb *)ctx;
    usb_hal_handle_t *h = u->hal;
    uint32_t gint = usb_hal_gintsts(h);

    if (gint & USB_HAL_GINT_USBRST) {
        usb_hal_gint_clear(h, USB_HAL_GINT_USBRST);
        usb_on_usb_reset(u);
        return;
    }
    if (gint & USB_HAL_GINT_ENUMDNE) {
        usb_hal_gint_clear(h, USB_HAL_GINT_ENUMDNE);
        return;
    }
    if (gint & USB_HAL_GINT_RXFLVL) { usb_handle_rx(u); return; }
    if (gint & USB_HAL_GINT_OEPINT)  { usb_handle_oep(u); return; }
    if (gint & USB_HAL_GINT_IEPINT)  { usb_handle_iep(u); return; }

    uint32_t misc = gint & (USB_HAL_GINT_SOF | USB_HAL_GINT_USBSUSP | USB_HAL_GINT_WKUP);
    if (misc) usb_hal_gint_clear(h, misc);
}

/* Re-arm everything after a USB reset (or at open): address 0, all endpoints
 * configured, EP0 OUT armed to receive the first SETUP. */
static void usb_on_usb_reset(usb *u)
{
    u->address = 0; u->config = 0; u->connected = 0; u->pending_address = 0;
    u->ep0_in_rem = 0;
    usb_hal_set_address(u->hal, 0);
    usb_hal_ep_config(u->hal, 0, 1, 64, USB_EP_TYPE_CTRL);
    usb_hal_ep_config(u->hal, 0, 0, 64, USB_EP_TYPE_CTRL);
    usb_hal_ep_config(u->hal, 1, 1, 64, USB_EP_TYPE_BULK);
    usb_hal_ep_config(u->hal, 1, 0, 64, USB_EP_TYPE_BULK);
    usb_hal_ep_config(u->hal, 2, 1, 10, USB_EP_TYPE_INT);
    usb_hal_ep_rx(u->hal, 0, 64);
}

/* ---- stream data path ------------------------------------------------ */

static int usb_stream_write(stream_device *self, const void *buf, size_t len)
{
    usb *u = (usb *)self;
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        uint16_t n = (len - done) < 64 ? (uint16_t)(len - done) : 64;
        while (u->bulk_tx_pending) { /* wait for prior bulk IN to finish */ }
        usb_hal_ep_tx(u->hal, 1, p + done, n);
        u->bulk_tx_pending = 1;
        /* wait for completion (poll the HW flag; safe in both POLL and IRQ) */
        while (u->bulk_tx_pending) {
            uint32_t m = usb_hal_diepint(u->hal, 1);
            if (m & USB_OTG_DIEPINT_XFRC_Msk) {
                usb_hal_diepint_clear(u->hal, 1, USB_OTG_DIEPINT_XFRC_Msk);
                u->bulk_tx_pending = 0;
            }
        }
        done += n;
    }
    return (int)done;
}

static int usb_stream_read(stream_device *self, void *buf, size_t len)
{
    ringbuffer *rb = stream_device_get_ringbuffer(self);
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        if (!rb || rb->fun->is_empty(rb)) break;   /* non-blocking: return what we have */
        rb->fun->get(rb, &p[done]);
        done++;
    }
    return (int)done;
}

static int usb_stream_flush(stream_device *self) { (void)self; return 0; }
static int usb_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }
static int usb_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }

/* ---- device interface ------------------------------------------------ */

static int usb_dev_open(device *self)
{
    usb *u = (usb *)self;

    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        pinmux_port_t port; uint8_t pin, af;
        pinmux_pin_cfg_t cfg = { .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };
        if (!pinmux_hal_resolve(u->dm_signal, &port, &pin, &af)) {
            printf("[usb] %s: unknown DM \"%s\"\r\n", u->parent.parent.name, u->dm_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            printf("[usb] %s: DM P%c%d CONFLICT\r\n", u->parent.parent.name, 'A' + port, pin);
            return -2;
        }
        cfg.af = af; pm->fun->config(pm, port, pin, &cfg);

        if (!pinmux_hal_resolve(u->dp_signal, &port, &pin, &af)) {
            printf("[usb] %s: unknown DP \"%s\"\r\n", u->parent.parent.name, u->dp_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            printf("[usb] %s: DP P%c%d CONFLICT\r\n", u->parent.parent.name, 'A' + port, pin);
            return -2;
        }
        cfg.af = af; pm->fun->config(pm, port, pin, &cfg);
    }

    usb_hal_enable_clock(u->hal);
    usb_hal_core_init(u->hal, 0 /* ignore VBUS sense */);
    usb_hal_gint_clear(u->hal, 0xFFFFFFFF);     /* clear any stale interrupts */

    irq_id_t id = usb_hal_irq_id(u->hal);
    irq_set_priority(id, 1);
    irq_manager_attach(id, usb_isr, u);
    irq_manager_enable(id, usb_isr, u);

    usb_on_usb_reset(u);
    usb_hal_connect(u->hal);                    /* pull DP up -> connect */

    stream_device_init_ringbuffer((stream_device *)u, u->rx_storage, USB_RX_BUF_SIZE);
    return 0;
}

static int usb_dev_close(device *self)
{
    usb *u = (usb *)self;
    irq_id_t id = usb_hal_irq_id(u->hal);
    irq_manager_detach(id, usb_isr, u);
    usb_hal_disconnect(u->hal);
    return 0;
}

static int usb_dev_read(device *self, void *buf, size_t len)
    { return usb_stream_read((stream_device *)self, buf, len); }
static int usb_dev_write(device *self, const void *buf, size_t len)
    { return usb_stream_write((stream_device *)self, buf, len); }

/* Host-free control-protocol self-test: feed synthetic SETUP packets and
 * compare the produced EP0 IN responses / side effects against the descriptors.
 * Returns 0 if every check passes, -1 otherwise. */
static int usb_cmp(const uint8_t *a, const uint8_t *b, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void usb_feed_setup(usb *u, const uint8_t *setup8,
                           const uint8_t *out_data, uint16_t out_len)
{
    memcpy(u->setup, setup8, 8);
    u->test_ep0_in_len = 0;
    u->ctrl_out_kind = 0;
    usb_ctrl_dispatch(u);
    if (u->ctrl_out_kind == 1 && out_data) {    /* OUT-data request (SET_LINE_CODING) */
        memcpy(u->ctrl_buf, out_data, out_len < 64 ? out_len : 64);
        if (out_len >= 7) {
            memcpy(u->line_coding, out_data, 7);
            u->ctrl_out_len = out_len;
        }
        u->ctrl_out_kind = 0;
        usb_hal_ep0_tx_zlp(u->hal);             /* status IN (harmless in test mode) */
    }
}

static int usb_run_ctrl_selftest(usb *u)
{
    int ok = 1;
    u->test_mode = 1;
    uint8_t setup[8];

    /* (1) GET_DESCRIPTOR(Device) */
    setup[0]=0x80; setup[1]=0x06; setup[2]=0x00; setup[3]=0x01;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0xFF; setup[7]=0x00;
    u->test_ep0_in_len = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_dev = (u->test_ep0_in_len == DEV_DESC_LEN) &&
                 usb_cmp(u->test_ep0_in, dev_desc, DEV_DESC_LEN);
    if (!ok_dev) ok = 0;
    printf("       ctrl GET_DESCRIPTOR(device): len=%u expect=%u %s\r\n",
           u->test_ep0_in_len, DEV_DESC_LEN, ok_dev ? "PASS" : "FAIL");

    /* (2) GET_DESCRIPTOR(Config) */
    setup[2]=0x00; setup[3]=0x02;
    u->test_ep0_in_len = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_cfg = (u->test_ep0_in_len == CFG_DESC_LEN) &&
                 usb_cmp(u->test_ep0_in, cfg_desc, CFG_DESC_LEN);
    if (!ok_cfg) ok = 0;
    printf("       ctrl GET_DESCRIPTOR(config): len=%u expect=%u %s\r\n",
           u->test_ep0_in_len, CFG_DESC_LEN, ok_cfg ? "PASS" : "FAIL");

    /* (3) GET_DESCRIPTOR(String index 0) -> langid 0x0409 */
    setup[3]=0x03;
    u->test_ep0_in_len = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_str = (u->test_ep0_in_len == 4) &&
                 (u->test_ep0_in[0]==4) && (u->test_ep0_in[1]==0x03) &&
                 (u->test_ep0_in[2]==0x09) && (u->test_ep0_in[3]==0x04);
    if (!ok_str) ok = 0;
    printf("       ctrl GET_DESCRIPTOR(string0): langid=0x%02X%02X %s\r\n",
           u->test_ep0_in[3], u->test_ep0_in[2], ok_str ? "PASS" : "FAIL");

    /* (4) GET_LINE_CODING -> default 115200 8N1 */
    setup[0]=0xA1; setup[1]=0x21; setup[2]=0x00; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x07; setup[7]=0x00;
    u->test_ep0_in_len = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_glc = (u->test_ep0_in_len == 7) &&
                 usb_cmp(u->test_ep0_in, u->line_coding, 7);
    if (!ok_glc) ok = 0;
    printf("       ctrl GET_LINE_CODING: 0x%02X%02X%02X%02X %s\r\n",
           u->test_ep0_in[3], u->test_ep0_in[2], u->test_ep0_in[1], u->test_ep0_in[0],
           ok_glc ? "PASS" : "FAIL");

    /* (5) SET_LINE_CODING 9600 8N1, then GET_LINE_CODING echoes it */
    uint8_t lc[7] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* 9600 LE */
    setup[0]=0x21; setup[1]=0x20; setup[6]=0x07;
    usb_feed_setup(u, setup, lc, 7);
    setup[0]=0xA1; setup[1]=0x21;
    u->test_ep0_in_len = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_slc = (u->test_ep0_in_len == 7) && usb_cmp(u->test_ep0_in, lc, 7);
    if (!ok_slc) ok = 0;
    printf("       ctrl SET/GET_LINE_CODING: 9600=%s\r\n", ok_slc ? "PASS" : "FAIL");

    /* (6) SET_CONTROL_LINE_STATE (DTR) */
    setup[0]=0x21; setup[1]=0x22; setup[2]=0x01; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_cls = (u->line_state & 0x1) ? 1 : 0;
    if (!ok_cls) ok = 0;
    printf("       ctrl SET_CONTROL_LINE_STATE: DTR=%s\r\n", ok_cls ? "PASS" : "FAIL");

    /* (7) SET_ADDRESS latched (applied on status IN completion) */
    setup[0]=0x00; setup[1]=0x05; setup[2]=0x07; setup[3]=0x00;
    setup[4]=0x00; setup[5]=0x00; setup[6]=0x00; setup[7]=0x00;
    u->pending_address = 0;
    usb_feed_setup(u, setup, NULL, 0);
    int ok_addr = (u->pending_address == 0x07);
    if (!ok_addr) ok = 0;
    printf("       ctrl SET_ADDRESS(7): latched=%s\r\n", ok_addr ? "PASS" : "FAIL");

    u->test_mode = 0;
    u->pending_address = 0;
    return ok ? 0 : -1;
}

static int usb_dev_ioctl(device *self, int cmd, void *arg)
{
    usb *u = (usb *)self;
    switch (cmd) {
    case USB_IOCTL_GET_GINTSTS: if (arg) *(uint32_t *)arg = usb_hal_gintsts_raw(u->hal); return 0;
    case USB_IOCTL_GET_GCCFG:   if (arg) *(uint32_t *)arg = usb_hal_gccfg(u->hal);          return 0;
    case USB_IOCTL_GET_DSTS:    if (arg) *(uint32_t *)arg = usb_hal_dsts(u->hal);           return 0;
    case USB_IOCTL_GET_ADDRESS: if (arg) *(uint32_t *)arg = u->address;             return 0;
    case USB_IOCTL_CONNECTED:   if (arg) *(uint32_t *)arg = (uint32_t)u->connected; return 0;
    case USB_IOCTL_SET_LINE_CODING:
        if (!arg) return -1;
        memcpy(u->line_coding, (const uint8_t *)arg, 7); return 0;
    case USB_IOCTL_GET_LINE_CODING:
        if (!arg) return -1;
        memcpy((uint8_t *)arg, u->line_coding, 7); return 0;
    case USB_IOCTL_RUN_CTRL_SELFTEST:
        return usb_run_ctrl_selftest(u);
    default:
        return -1;
    }
}
