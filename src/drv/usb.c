#include "usb.h"
#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include "hal/stm32/usb_hal.h"
#include "irq_manager.h"
#include "common/ringbuffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- CDC descriptors -------------------------------------------------- */

/* Device descriptor (18 bytes). Miscellaneous class + IAD so Windows loads the
 * native usbser driver without an INF.
 * NOTE: intentionally NOT `const` — in OTG-DMA mode the core's built-in DMA
 * reads this buffer directly (DIEPDMA) during GET_DESCRIPTOR, and the OTG FS
 * DMA master cannot read Flash (0x08000000). Keeping it in .data (main SRAM)
 * makes it DMA-readable; the initializer is copied from Flash at startup. */
static uint8_t dev_desc[18] = {
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

    /* CDC line coding / state (7 bytes: 4B baud LE, 1B stop, 1B parity, 1B data) */
    uint8_t  line_coding[7];
    uint8_t  line_state;           /* DTR/RTS from SET_CONTROL_LINE_STATE */

    volatile int bulk_tx_pending;   /* 1 => bulk-IN endpoint busy (XFRC pending) */

    /* TX staging ring: usb_stream_write() stages bytes here (non-blocking) and
     * usb_tx_pump() arms the bulk-IN endpoint from it. Decouples the producer
     * (main loop) from IN pacing so the loop never stalls waiting for the host
     * to read, and the IN endpoint is only ever armed while idle. */
    ringbuffer *tx_rb;
    uint8_t  rx_storage[USB_RX_BUF_SIZE];
    uint8_t  tx_storage[USB_TX_BUF_SIZE];

    /* DMA-safe staging buffer for the bulk-IN source. usb_tx_pump copies a
     * chunk from the TX ring into HERE and hands it to DCD_EP_Tx. In OTG-DMA
     * mode the OTG's built-in DMA reads this buffer directly, so it MUST live
     * in main SRAM — a stack buffer would sit in CCM (0x10000000), which the
     * OTG DMA cannot access. The struct is malloc'd from the main-SRAM heap,
     * so this field is DMA-safe. In slave/FIFO mode the CPU copies it into the
     * TX FIFO, which is also fine. */
    uint8_t  tx_dma_buf[64];

    /* host-free self-test mode */
    int      test_mode;
    uint8_t  test_ep0_in[128];
    uint16_t test_ep0_in_len;

    int      dbg_print;

    /* ISR / enumeration event counters for USBSTAT */
    uint32_t dbg_irq;
    uint32_t dbg_rst;
    uint32_t dbg_enum;
    uint32_t dbg_setup;
    uint32_t dbg_out;
    uint32_t dbg_in;
    uint32_t dbg_setaddr;

    uint8_t  dbg_ep0_in[64];
    uint8_t  dbg_ep0_in_len;
};

/* Single instance handle so the CDC class accessors (defined here) can reach
 * the driver object. */
static struct _usb *g_usb = 0;

/* ---- vtable forward decls --------------------------------------------- */

static int  usb_dev_open(device *self);
static int  usb_dev_close(device *self);
static int  usb_dev_read(device *self, void *buf, size_t len);
static int  usb_dev_write(device *self, const void *buf, size_t len);
static int  usb_dev_ioctl(device *self, int cmd, void *arg);
static int  usb_dev_irq_id(device *self);

static int  usb_stream_read(stream_device *self, void *buf, size_t len);
static int  usb_stream_write(stream_device *self, const void *buf, size_t len);
static int  usb_stream_flush(stream_device *self);
static int  usb_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta);
static int  usb_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta);

static void usb_isr(void *ctx);

/* forward decl: arms bulk-IN from the TX staging ring (defined in stream path) */
static void usb_tx_pump(usb *u);

static const struct stream_deviceVtable usb_stream_vtable = {
    .read        = usb_stream_read,
    .write       = usb_stream_write,
    .flush       = usb_stream_flush,
    .read_frame  = usb_stream_read_frame,
    .write_frame = usb_stream_write_frame,
    .submit      = NULL,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable usb_dev_vtable = {
    .open  = usb_dev_open,
    .close = usb_dev_close,
    .read  = usb_dev_read,
    .write = usb_dev_write,
    .ioctl = usb_dev_ioctl,
    .irq_id = usb_dev_irq_id,
};

/* ---- CDC class accessors (called from usbd_cdc_core.c) ---------------- */

void usbd_cdc_register_usb(struct _usb *u) { g_usb = u; }

uint8_t *usbd_cdc_line_coding_ptr(void)
{
    return g_usb ? g_usb->line_coding : 0;
}

void usbd_cdc_on_line_state(uint8_t s)
{
    if (g_usb) g_usb->line_state = s;
}

void usbd_cdc_rx_push(const uint8_t *data, uint16_t len)
{
    ringbuffer *rb = g_usb ? stream_device_get_ringbuffer((stream_device *)g_usb) : 0;
    for (uint16_t i = 0; i < len && rb; i++)
        rb->fun->put(rb, data[i]);
    if (g_usb) g_usb->dbg_out++;   /* count bulk-OUT completions */
}

size_t usbd_cdc_rx_room(void)
{
    ringbuffer *rb = g_usb ? stream_device_get_ringbuffer((stream_device *)g_usb) : 0;
    return rb ? rb->fun->free_space(rb) : 0;
}

void usbd_cdc_tx_done(void)
{
    /* IN transfer finished (XFRC). ONLY clear the flag here — do NOT touch the
     * TX staging ring. The ring has a SINGLE consumer (the main loop, which
     * calls usb_tx_pump from usb_stream_write / the per-iteration TX_PUMP
     * ioctl). If the ISR also consumed the ring, we'd have two consumers on a
     * single-consumer ringbuffer and corrupt its tail index under load (the
     * intermittent byte errors seen on large transfers). The main loop pumps
     * the next chunk on its very next iteration (sub-microsecond latency). */
    if (g_usb) { g_usb->bulk_tx_pending = 0; g_usb->dbg_in++; }
}

void usb_cdc_apply_line_coding(const uint8_t *buf, uint16_t len)
{
    if (!g_usb) return;
    uint16_t n = len < 7 ? len : 7;
    for (uint16_t i = 0; i < n; i++)
        g_usb->line_coding[i] = buf[i];
}

/* ---- USBD_DEVICE descriptor callbacks (standard requests) ------------- */

static uint8_t usb_str_buf[64];

static uint8_t *usb_get_device_descriptor(uint8_t speed, uint16_t *len)
{
    (void)speed; *len = DEV_DESC_LEN; return (uint8_t *)dev_desc;
}

static uint8_t usb_langid_desc[4] = { 4, 0x03, 0x09, 0x04 };
static uint8_t *usb_get_langid(uint8_t speed, uint16_t *len)
{
    (void)speed; *len = 4; return usb_langid_desc;
}
static uint8_t *usb_get_mfr(uint8_t speed, uint16_t *len)
{
    (void)speed; USBD_GetString((uint8_t *)"joc-base", usb_str_buf, len); return usb_str_buf;
}
static uint8_t *usb_get_product(uint8_t speed, uint16_t *len)
{
    (void)speed; USBD_GetString((uint8_t *)"CDC-ACM", usb_str_buf, len); return usb_str_buf;
}
static uint8_t *usb_get_serial(uint8_t speed, uint16_t *len)
{
    (void)speed; USBD_GetString((uint8_t *)"JOC0001", usb_str_buf, len); return usb_str_buf;
}

static USBD_DEVICE g_usr_device = {
    .GetDeviceDescriptor        = usb_get_device_descriptor,
    .GetLangIDStrDescriptor     = usb_get_langid,
    .GetManufacturerStrDescriptor = usb_get_mfr,
    .GetProductStrDescriptor    = usb_get_product,
    .GetSerialStrDescriptor     = usb_get_serial,
    .GetConfigurationStrDescriptor = 0,
    .GetInterfaceStrDescriptor  = 0,
};

/* ---- USBD_Usr_cb_TypeDef (connect/reset/config callbacks) ------------- */

static void usb_usr_init(void) {}
static void usb_usr_reset(uint8_t speed)
{
    (void)speed;
    if (g_usb) { g_usb->dbg_rst++; g_usb->connected = 0; g_usb->config = 0; }
}
static void usb_usr_configured(void)
{
    if (g_usb) { g_usb->connected = 1; g_usb->config = 1; }
}
static void usb_usr_suspended(void) {}
static void usb_usr_resumed(void) {}
static void usb_usr_connected(void)  { if (g_usb) g_usb->connected = 1; }
static void usb_usr_disconnected(void) { if (g_usb) g_usb->connected = 0; }

static USBD_Usr_cb_TypeDef g_cdc_usr_cb = {
    .Init            = usb_usr_init,
    .DeviceReset     = usb_usr_reset,
    .DeviceConfigured= usb_usr_configured,
    .DeviceSuspended = usb_usr_suspended,
    .DeviceResumed   = usb_usr_resumed,
    .DeviceConnected = usb_usr_connected,
    .DeviceDisconnected = usb_usr_disconnected,
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

    g_usb = p;
    usbd_cdc_register_usb(p);

    /* Select OTG internal-DMA vs slave/FIFO mode BEFORE USBD_Init builds the
     * core cfg (USB_OTG_SelectCore reads usb_hal_get_dma_enable()). */
    usb_hal_set_dma_enable(c->dma_enable);


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
    p->dbg_print = 0;

    /* TX staging ring (device->host). Backed by p->tx_storage; overwrite OFF so
     * a stalled host exerts back-pressure instead of silently dropping. */
    ringbuffer_config_t txcfg = { .buf = p->tx_storage, .size = USB_TX_BUF_SIZE, .overwrite = 0 };
    p->tx_rb = ringbuffer_create(&txcfg);
    return (device *)p;
}

void usb_destroy(usb *self)
{
    if (!self) return;
    if (self->tx_rb) { ringbuffer_destroy(self->tx_rb); self->tx_rb = NULL; }
    stream_device_free_ringbuffer((stream_device *)self);
    usb_hal_destroy(self->hal);
    free(self);
}

/* ---- ISR -------------------------------------------------------------- */

static void usb_isr(void *ctx)
{
    usb *u = (usb *)ctx;
    USB_OTG_CORE_HANDLE *pdev = u->hal->pdev;
    uint32_t g = pdev->regs.GREGS->GINTSTS & pdev->regs.GREGS->GINTMSK;
    u->dbg_irq++;
    if (g & (1UL << 12)) u->dbg_rst++;    /* USBRST */
    if (g & (1UL << 13)) u->dbg_enum++;   /* ENUMDNE */
    /* Delegate all silicon handling to ST's verified OTG FS interrupt engine,
     * which drives the control/data state machine and our CDC class fops. */
    USBD_OTG_ISR_Handler(pdev);
}

/* ---- stream data path ------------------------------------------------ */

/* Arm at most ONE bulk-IN transfer at a time, draining the TX staging ring.
 * The single choke-point that touches DCD_EP_Tx, so the endpoint is only ever
 * armed while idle (bulk_tx_pending == 0). Re-arming an active IN endpoint was
 * the root cause of a permanent bulk-IN stall under sustained streaming: the
 * host occasionally is not ready within the old busy-wait window, the writer
 * gave up while the endpoint was still active, and the next DCD_EP_Tx corrupted
 * its state. This function is the TX ring's ONLY consumer and is called solely
 * from the main loop (usb_stream_write and the per-iteration USB_IOCTL_TX_PUMP);
 * the IN-complete ISR must NOT call it (it only clears bulk_tx_pending) so the
 * single-consumer ringbuffer invariant holds. In slave/FIFO mode DCD_EP_Tx
 * copies the bytes into the TX FIFO inline; in OTG-DMA mode it arms the OTG's
 * built-in DMA to read them from tx_dma_buf, so that buffer (main SRAM) must
 * stay valid for the whole IN transfer and must not be a stack/CCM buffer. */
static void usb_tx_pump(usb *u)
{
    if (!u || u->bulk_tx_pending)
        return;                                   /* IN busy: wait for XFRC */
    ringbuffer *rb = u->tx_rb;
    if (!rb || rb->fun->is_empty(rb))
        return;                                   /* nothing staged to send */
    /* Stage into the DMA-safe buffer (main SRAM). In OTG-DMA mode the OTG's
     * built-in DMA reads from this buffer directly; a stack buffer would be in
     * CCM and unreachable by the DMA. In slave/FIFO mode the CPU copies it into
     * the TX FIFO. Either way only ONE IN is armed at a time and bulk_tx_pending
     * guards re-entry, so this single buffer is never overwritten mid-transfer. */
    size_t n = rb->fun->read(rb, u->tx_dma_buf, sizeof(u->tx_dma_buf));
    if (n == 0)
        return;
    u->bulk_tx_pending = 1;                       /* set BEFORE arming */
    DCD_EP_Tx(u->hal->pdev, 0x81, u->tx_dma_buf, (uint16_t)n);
}

static int usb_stream_write(stream_device *self, const void *buf, size_t len)
{
    usb *u = (usb *)self;
    ringbuffer *rb = u->tx_rb;
    if (!rb) return -1;
    /* Stage the bytes (non-blocking). Overwrite is OFF, so if the host is not
     * draining fast enough this returns fewer than len and the caller applies
     * back-pressure instead of us silently losing data. */
    size_t stored = rb->fun->write(rb, buf, len);
    usb_tx_pump(u);                               /* arm IN if it is idle */
    return (int)stored;
}

static int usb_stream_read(stream_device *self, void *buf, size_t len)
{
    ringbuffer *rb = stream_device_get_ringbuffer(self);
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        if (!rb || rb->fun->is_empty(rb)) break;   /* non-blocking */
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
            log_printf(app_log(), LOG_DEBUG, "usb", "%s: unknown DM \"%s\"", u->parent.parent.name, u->dm_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "usb", "%s: DM P%c%d CONFLICT", u->parent.parent.name, 'A' + port, pin);
            return -2;
        }
        cfg.af = af; pm->fun->config(pm, port, pin, &cfg);

        if (!pinmux_hal_resolve(u->dp_signal, &port, &pin, &af)) {
            log_printf(app_log(), LOG_DEBUG, "usb", "%s: unknown DP \"%s\"", u->parent.parent.name, u->dp_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "usb", "%s: DP P%c%d CONFLICT", u->parent.parent.name, 'A' + port, pin);
            return -2;
        }
        cfg.af = af; pm->fun->config(pm, port, pin, &cfg);
    }

    usb_hal_enable_clock(u->hal);

    /* Bring up ST's OTG FS core + control engine, with our CDC class and user
     * callbacks. USB_OTG_BSP_Init (clocks + PA11/PA12) and DCD_Init (silicon)
     * happen inside USBD_Init. */
    USBD_Init(u->hal->pdev, USB_OTG_FS_CORE_ID, &g_usr_device, &USBD_CDC_cb, &g_cdc_usr_cb);

    irq_id_t id = usb_hal_irq_id(u->hal);
    irq_manager_set_priority(id, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    irq_manager_attach(id, usb_isr, u);
    irq_manager_enable(id, usb_isr, u);

    usb_hal_connect(u->hal);            /* pull DP up -> connect */
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

/* ---- host-free control-protocol self-test ---------------------------- */
/* Feeds synthetic SETUP packets through ST's standard-request engine and the
 * CDC class setup handler, and checks the queued response matches the
 * descriptors / line coding. Returns 0 if every check passes, -1 otherwise. */
static int usb_run_ctrl_selftest(usb *u)
{
    int ok = 1;
    USB_OTG_CORE_HANDLE *pdev = u->hal->pdev;
    USB_SETUP_REQ req;

    /* Mask the USB ISR for the whole synthetic control-transfer sequence. CDC
     * is normally LIVE (connected as a console), so the host keeps polling EP0;
     * its real SETUP/IN tokens would otherwise interleave with our synchronous
     * ST-request-engine calls and overwrite in_ep[0].xfer_buff (and advance it
     * for multi-packet descriptors) between a request and its check below,
     * making this host-free self-test racy. With the ISR masked the checks are
     * deterministic; the host merely retries its own control transfer after we
     * re-enable, so the live console is unaffected beyond a brief gap. */
    irq_id_t id = usb_hal_irq_id(u->hal);
    irq_manager_disable(id, usb_isr, u);
    uint8_t saved_status = pdev->dev.device_status;   /* restored after SET_ADDRESS */

    /* (1) GET_DESCRIPTOR(device) */
    uint8_t s[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0xFF, 0x00 };
    memcpy(pdev->dev.setup_packet, s, 8);
    USBD_ParseSetupRequest(pdev, &req);
    USBD_StdDevReq(pdev, &req);
    int ok_dev = (pdev->dev.in_ep[0].xfer_buff == (uint8_t *)dev_desc) &&
                 (pdev->dev.in_ep[0].xfer_len == DEV_DESC_LEN);
    if (!ok_dev) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl GET_DESCRIPTOR(device): len=%u expect=%u %s",
           (unsigned)pdev->dev.in_ep[0].xfer_len, (unsigned)DEV_DESC_LEN, ok_dev ? "PASS" : "FAIL");

    /* (2) GET_DESCRIPTOR(config) */
    s[2] = 0x00; s[3] = 0x02;
    memcpy(pdev->dev.setup_packet, s, 8);
    USBD_ParseSetupRequest(pdev, &req);
    USBD_StdDevReq(pdev, &req);
    /* NOTE: the config descriptor (75 B) spans >1 EP0 packet (MPS=64), so the
     * silicon caps in_ep[0].xfer_len to the first packet (64) inside
     * USB_OTG_EP0StartXfer. The FULL length lives in total_data_len/rem_data_len
     * and is delivered across subsequent IN tokens by the EP0 state machine. */
    int ok_cfg = (pdev->dev.in_ep[0].xfer_buff == cdc_config_descriptor) &&
                 (pdev->dev.in_ep[0].total_data_len == CFG_DESC_LEN);
    if (!ok_cfg) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl GET_DESCRIPTOR(config): len=%u expect=%u (xfer_len=%u) %s",
           (unsigned)pdev->dev.in_ep[0].total_data_len, (unsigned)CFG_DESC_LEN,
           (unsigned)pdev->dev.in_ep[0].xfer_len, ok_cfg ? "PASS" : "FAIL");

    /* (3) GET_DESCRIPTOR(string langid) -> 4-byte langid descriptor */
    s[3] = 0x03; s[2] = 0x00;
    memcpy(pdev->dev.setup_packet, s, 8);
    USBD_ParseSetupRequest(pdev, &req);
    USBD_StdDevReq(pdev, &req);
    int ok_str = (pdev->dev.in_ep[0].xfer_len == 4);
    if (!ok_str) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl GET_DESCRIPTOR(string0): len=%u expect=4 %s",
           (unsigned)pdev->dev.in_ep[0].xfer_len, ok_str ? "PASS" : "FAIL");

    /* (4) GET_LINE_CODING -> default 115200 8N1 */
    uint8_t sglc[8] = { 0xA1, 0x21, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00 };
    memcpy(pdev->dev.setup_packet, sglc, 8);
    USBD_ParseSetupRequest(pdev, &req);
    cdc_Setup(pdev, &req);
    int ok_glc = (pdev->dev.in_ep[0].xfer_buff == u->line_coding) &&
                 (pdev->dev.in_ep[0].xfer_len == 7);
    if (!ok_glc) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl GET_LINE_CODING: len=%u expect=7 %s",
           (unsigned)pdev->dev.in_ep[0].xfer_len, ok_glc ? "PASS" : "FAIL");

    /* (5) SET_LINE_CODING 9600 then GET_LINE_CODING echoes it */
    uint8_t lc[7] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* 9600 LE */
    uint8_t sslc[8] = { 0x21, 0x20, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00 };
    memcpy(pdev->dev.setup_packet, sslc, 8);
    USBD_ParseSetupRequest(pdev, &req);
    cdc_Setup(pdev, &req);             /* arms prepare-rx into cdc_cmd_buf */
    usbd_cdc_feed_cmd(lc, 7);          /* emulate host OUT data + RxReady */
    int ok_slc = (u->line_coding[0] == 0x80) && (u->line_coding[1] == 0x25);
    if (!ok_slc) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl SET/GET_LINE_CODING: 9600=%s", ok_slc ? "PASS" : "FAIL");

    /* (6) SET_CONTROL_LINE_STATE (DTR) */
    uint8_t scl[8] = { 0x21, 0x22, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memcpy(pdev->dev.setup_packet, scl, 8);
    USBD_ParseSetupRequest(pdev, &req);
    cdc_Setup(pdev, &req);
    int ok_cls = (u->line_state & 0x1) ? 1 : 0;
    if (!ok_cls) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl SET_CONTROL_LINE_STATE: DTR=%s", ok_cls ? "PASS" : "FAIL");

    /* (7) SET_ADDRESS latched into DCFG.DAD.
     * ST's engine only honors a synthetic SET_ADDRESS while the device is NOT
     * already CONFIGURED (a live host may have configured it). Force a
     * non-configured state for this single request so the check is meaningful;
     * the ISR is masked, so the live host never observes the transient. */
    pdev->dev.device_status = USB_OTG_ADDRESSED;
    uint8_t sadd[8] = { 0x00, 0x05, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 };
    memcpy(pdev->dev.setup_packet, sadd, 8);
    USBD_ParseSetupRequest(pdev, &req);
    USBD_StdDevReq(pdev, &req);
    uint8_t dad = (uint8_t)((usb_hal_dcfg(u->hal) >> 4) & 0x7FUL);
    int ok_addr = (dad == 0x07);
    if (!ok_addr) ok = 0;
    log_printf(app_log(), LOG_DEBUG, "usb", "       ctrl SET_ADDRESS(7): DAD=0x%02X %s", dad, ok_addr ? "PASS" : "FAIL");
    pdev->dev.device_status = saved_status;          /* restore live state */
    DCD_EP_SetAddress(pdev, 0);        /* restore so real enumeration is clean */
    if (usb_hal_get_dma_enable()) {
        /* In DMA mode the synthetic control transfers above armed the OTG's
         * built-in DMA on EP0 (with the ISR masked, so XFRC never fired). Flush
         * EP0 IN/OUT so no stale DMA is pending when a real host later
         * enumerates — otherwise the endpoint can wedge the enumeration. */
        DCD_EP_Flush(pdev, 0x80);
        DCD_EP_Flush(pdev, 0x00);
    }
    irq_manager_enable(id, usb_isr, u);/* let the live host resume EP0 traffic */

    return ok ? 0 : -1;
}

/* ---- ioctl ------------------------------------------------------------- */

static int usb_dev_ioctl(device *self, int cmd, void *arg)
{
    usb *u = (usb *)self;
    switch (cmd) {
    case USB_IOCTL_GET_GINTSTS: if (arg) *(uint32_t *)arg = usb_hal_gintsts_raw(u->hal); return 0;
    case USB_IOCTL_GET_GCCFG:   if (arg) *(uint32_t *)arg = usb_hal_gccfg(u->hal);          return 0;
    case USB_IOCTL_GET_DSTS:    if (arg) *(uint32_t *)arg = usb_hal_dsts(u->hal);           return 0;
    case USB_IOCTL_GET_ADDRESS: {
        uint8_t dad = (uint8_t)((usb_hal_dcfg(u->hal) >> 4) & 0x7FUL);
        if (arg) *(uint32_t *)arg = dad;
        return 0;
    }
    case USB_IOCTL_CONNECTED:   if (arg) *(uint32_t *)arg = (uint32_t)u->connected; return 0;
    case USB_IOCTL_TX_FREE: {
        /* Bytes the TX staging ring can still accept — used by the echo loop to
         * cap its read so it never pulls more from RX than it can stage. */
        size_t free = u->tx_rb ? u->tx_rb->fun->free_space(u->tx_rb) : 0;
        if (arg) *(size_t *)arg = free;
        return 0;
    }
    case USB_IOCTL_TX_PUMP:
        /* Drain the TX staging ring into the bulk-IN endpoint. Safe to call from
         * the main loop (the ring's only consumer); must NOT be called from the
         * IN-complete ISR. */
        usb_tx_pump(u);
        return 0;
    case USB_IOCTL_RX_REARM:
        /* Re-arm the bulk-OUT endpoint if it was NAK'd for lack of RX-ring room.
         * Called from the main loop after draining RX, restoring flow once space
         * is available (USB back-pressure, no silent drops). */
        usbd_cdc_out_reenarm();
        return 0;
    case USB_IOCTL_SET_LINE_CODING:
        if (!arg) return -1;
        memcpy(u->line_coding, (const uint8_t *)arg, 7); return 0;
    case USB_IOCTL_GET_LINE_CODING:
        if (!arg) return -1;
        memcpy((uint8_t *)arg, u->line_coding, 7); return 0;
    case USB_IOCTL_RUN_CTRL_SELFTEST:
        return usb_run_ctrl_selftest(u);
    case USB_IOCTL_DBG_SET:
        u->dbg_print = arg ? *(const int *)arg : 0;
        return 0;
    case USB_IOCTL_DBG_DUMP: {
        uint32_t gint = usb_hal_gintsts_raw(u->hal);
        uint32_t dctl = usb_hal_dctl(u->hal);
        uint32_t dsts = usb_hal_dsts(u->hal);
        uint32_t gccf = usb_hal_gccfg(u->hal);
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "irq=%lu rst=%lu enum=%lu setup=%lu out=%lu in=%lu",
                   (unsigned long)u->dbg_irq, (unsigned long)u->dbg_rst,
                   (unsigned long)u->dbg_enum, (unsigned long)u->dbg_setup,
                   (unsigned long)u->dbg_out, (unsigned long)u->dbg_in);
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "GINTSTS=0x%08lX GCCFG=0x%08lX DCTL=0x%08lX DSTS=0x%08lX",
                   (unsigned long)gint, (unsigned long)gccf,
                   (unsigned long)dctl, (unsigned long)dsts);
        /* Decode the MASKED (i.e. actually pending & firing) interrupt bits to
         * localize an interrupt storm. Bit positions from stm32f4 ref manual. */
        {
            uint32_t m = gint & u->hal->pdev->regs.GREGS->GINTMSK;
            char bits[96] = "";
            if (m & (1UL<<4))  strcat(bits, " RXFLVL");
            if (m & (1UL<<5))  strcat(bits, " NPTXFE");
            if (m & (1UL<<11)) strcat(bits, " USBSUSP");
            if (m & (1UL<<12)) strcat(bits, " USBRST");
            if (m & (1UL<<13)) strcat(bits, " ENUMDNE");
            if (m & (1UL<<15)) strcat(bits, " EOPF");
            if (m & (1UL<<18)) strcat(bits, " IEPINT");
            if (m & (1UL<<19)) strcat(bits, " OEPINT");
            if (m & (1UL<<3))  strcat(bits, " SOF");
            if (m & (1UL<<26)) strcat(bits, " CIDSCHG");
            if (m & (1UL<<30)) strcat(bits, " SRQINT");
            log_printf(app_log(), LOG_DEBUG, "usb",
                       "GINT(masked)=0x%08lX:%s DIEPEMPMSK=0x%08lX bulk_tx_pending=%d g_out_nak=%d",
                       (unsigned long)m, bits,
                       (unsigned long)u->hal->pdev->regs.DREGS->DIEPEMPMSK,
                       (int)u->bulk_tx_pending, (int)usbd_cdc_out_nak());
        }
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "addr=%u cfg=%u connected=%d line_state=0x%02X",
                   (unsigned)(uint8_t)((usb_hal_dcfg(u->hal) >> 4) & 0x7FUL),
                   (unsigned)u->config, u->connected, (unsigned)u->line_state);

        /* CDC line coding (baud is VIRTUAL for USB VCP; stored, not timed).
         * Lets a host confirm the baud it set via SET_LINE_CODING was received. */
        uint32_t baud = (uint32_t)(u->line_coding[0] | (u->line_coding[1] << 8) |
                                   (u->line_coding[2] << 16) | (u->line_coding[3] << 24));
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "line_coding: baud=%lu stop=%u parity=%u data=%u (8N1 default)",
                   (unsigned long)baud, (unsigned)u->line_coding[4],
                   (unsigned)u->line_coding[5], (unsigned)u->line_coding[6]);

        uint32_t gusb = usb_hal_gusbcfg(u->hal);
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "GUSBCFG=0x%08lX (FDMOD=%lu PHYSEL=%lu TRDT=%lu)",
                   (unsigned long)gusb, (unsigned long)((gusb >> 30) & 1),
                   (unsigned long)((gusb >> 6) & 1), (unsigned long)((gusb >> 10) & 0xF));
        uint32_t dcfg = usb_hal_dcfg(u->hal);
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "DCFG=0x%08lX DSPD=%lu DAD=%lu",
                   (unsigned long)dcfg, (unsigned long)(dcfg & 3),
                   (unsigned long)((dcfg >> 4) & 0x7FUL));

        uint32_t diep0 = usb_hal_diepctl(u->hal, 0);
        uint32_t doep0 = usb_hal_doepctl(u->hal, 0);
        log_printf(app_log(), LOG_DEBUG, "usb",
                   "EP0 DIEPCTL=0x%08lX DOEPCTL=0x%08lX MPSIZ=%u/%u",
                   (unsigned long)diep0, (unsigned long)doep0,
                   (unsigned)(diep0 & 0x3UL), (unsigned)(doep0 & 0x3UL));

        /* EP1 bulk IN/OUT state — to localize a streaming stall (OUT not armed
         * vs IN stuck). xfer_count = bytes moved so far on that endpoint. */
        {
            USB_OTG_CORE_HANDLE *pdev = u->hal->pdev;
            uint32_t diep1 = usb_hal_diepctl(u->hal, 1);
            uint32_t doep1 = usb_hal_doepctl(u->hal, 1);
            log_printf(app_log(), LOG_DEBUG, "usb",
                       "EP1 IN  DIEPCTL=0x%08lX xfer_count=%u xfer_len=%u",
                       (unsigned long)diep1,
                       (unsigned)pdev->dev.in_ep[1].xfer_count,
                       (unsigned)pdev->dev.in_ep[1].xfer_len);
            log_printf(app_log(), LOG_DEBUG, "usb",
                       "EP1 OUT DOEPCTL=0x%08lX xfer_count=%u xfer_len=%u",
                       (unsigned long)doep1,
                       (unsigned)pdev->dev.out_ep[1].xfer_count,
                       (unsigned)pdev->dev.out_ep[1].xfer_len);
        }

        /* DECISIVE TEST: can DCFG.DAD be written at all, and does it stick?
         * Write a few distinct addresses, read each back, then restore. */
        {
            uint8_t save = (uint8_t)((usb_hal_dcfg(u->hal) >> 4) & 0x7FUL);
            uint8_t probes[] = { 5, 9, 17, 0x7F };
            log_printf(app_log(), LOG_DEBUG, "usb",
                       "DCFG.DAD write test (immediate readback):");
            for (int t = 0; t < 4; t++) {
                DCD_EP_SetAddress(u->hal->pdev, probes[t]);
                uint8_t rb = (uint8_t)((usb_hal_dcfg(u->hal) >> 4) & 0x7FUL);
                log_printf(app_log(), LOG_DEBUG, "usb",
                           "      wrote=%u readback=%u %s",
                           (unsigned)probes[t], (unsigned)rb,
                           (rb == probes[t]) ? "OK" : "FAIL");
            }
            DCD_EP_SetAddress(u->hal->pdev, save);
            log_printf(app_log(), LOG_DEBUG, "usb",
                       "      restored DAD=%u", (unsigned)save);
        }
        return 0;
    }
    default:
        return -1;
    }
}

static int usb_dev_irq_id(device *self)
{
    usb *u = (usb *)self;
    return (int)usb_hal_irq_id(u->hal);
}
