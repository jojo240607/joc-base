#include "spi.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"
#include "pinmux_hal.h"
#include "irq_manager.h"             /* irq_manager_attach / enable / disable */
#include "drv/dma.h"                  /* dma device + dma_hal_route (DMA engine) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/* virtual implementations dispatched through the unified device vtable */
static int spi_dev_open(device *self);
static int spi_dev_close(device *self);
static int spi_dev_read(device *self, void *buf, size_t len);
static int spi_dev_write(device *self, const void *buf, size_t len);
static int spi_dev_ioctl(device *self, int cmd, void *arg);

/* stream-class vtable (read/write/flush/submit) */
static int spi_stream_read(stream_device *self, void *buf, size_t len);
static int spi_stream_write(stream_device *self, const void *buf, size_t len);
static int spi_stream_flush(stream_device *self);
static int spi_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta);
static int spi_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta);

/* internal: do a full-duplex transfer in the current mode */
static int spi_do_xfer(spi *p, const uint8_t *tx, uint8_t *rx, uint16_t len);

/* ISR (registered via the irq framework) */
static void spi_isr(void *ctx);

static const struct stream_deviceVtable spi_stream_vtable = {
    .read        = spi_stream_read,
    .write       = spi_stream_write,
    .flush       = spi_stream_flush,
    .read_frame  = spi_stream_read_frame,
    .write_frame = spi_stream_write_frame,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable spi_dev_vtable = {
    .open  = spi_dev_open,
    .close = spi_dev_close,
    .read  = spi_dev_read,
    .write = spi_dev_write,
    .ioctl = spi_dev_ioctl,
};

device *spi_create(const void *config)
{
    const spi_config_t *c = (const spi_config_t *)config;
    if (!c || !c->sck_signal || !c->miso_signal || !c->mosi_signal)
        return NULL;

    spi *p = (spi *)malloc(sizeof(spi));
    if (!p) return NULL;
    memset(p, 0, sizeof(spi));

    pinmux_port_t sp; uint8_t spn, saf;
    if (!pinmux_hal_resolve(c->sck_signal, &sp, &spn, &saf)) {
        log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: unknown SCK \"%s\"\n", c->name, c->sck_signal);
        free(p); return NULL;
    }
    pinmux_port_t mp; uint8_t mpn, maf;
    if (!pinmux_hal_resolve(c->miso_signal, &mp, &mpn, &maf)) {
        log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: unknown MISO \"%s\"\n", c->name, c->miso_signal);
        free(p); return NULL;
    }
    pinmux_port_t dp; uint8_t dpn, daf;
    if (!pinmux_hal_resolve(c->mosi_signal, &dp, &dpn, &daf)) {
        log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: unknown MOSI \"%s\"\n", c->name, c->mosi_signal);
        free(p); return NULL;
    }
    if (saf != maf || saf != daf) {
        log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: AF mismatch SCK=%u MISO=%u MOSI=%u\n",
               c->name, (unsigned)saf, (unsigned)maf, (unsigned)daf);
        free(p); return NULL;
    }

    p->hal = spi_hal_create(c->peripheral);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &spi_dev_vtable;
    p->parent.vtable        = &spi_stream_vtable;
    p->parent.parent.type   = DEVICE_TYPE_SPI;
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.parent.name   = c->name;
    p->parent.mode          = STREAM_MODE_POLL;   /* default */
    p->pclk_hz = c->pclk_hz;
    p->baud_hz = c->baud_hz;
    p->sck_port = sp; p->sck_pin = spn; p->sck_af = saf;
    p->miso_port = mp; p->miso_pin = mpn; p->miso_af = maf;
    p->mosi_port = dp; p->mosi_pin = dpn; p->mosi_af = daf;
    p->irq = spi_hal_irq_id(p->hal);
    p->dma_tx_req = c->dma_tx_req;        /* cache DMA request IDs for open() */
    p->dma_rx_req = c->dma_rx_req;

    return &p->parent.parent;
}

void spi_destroy(spi *self)
{
    if (!self) return;
    spi_hal_destroy(self->hal);
    free(self);
}

/* --- DMA engine (STREAM_MODE_DMA) ---
 * SPI is full-duplex: a master transfer needs BOTH the TX and RX streams armed
 * at once (otherwise the un-drained RX side raises an Overrun). The driver
 * reserves both hard-wired streams at open() (via dma_hal_route) and keeps them
 * until close(). The DMA ISR ( drv/dma.c) gives the stream done_sem on TC/TE. */
static int spi_dma_acquire(spi *p)
{
    p->dma_dev = NULL; p->dma_tx = NULL; p->dma_rx = NULL;
    if (p->dma_tx_req == DMA_REQ_NONE || p->dma_rx_req == DMA_REQ_NONE)
        return -1;   /* SPI needs both directions for full-duplex DMA */
    dma_route_t rt_tx = dma_hal_route(p->dma_tx_req);
    dma_route_t rt_rx = dma_hal_route(p->dma_rx_req);
    if (!rt_tx.name || !rt_rx.name) return -1;
    dma *dm = (dma *)device_manager_get(rt_tx.name);
    if (!dm) return -1;
    dma_stream_t *stx = dm->fun->acquire(dm, rt_tx.stream, rt_tx.channel, DMA_DIR_M2P);
    dma_stream_t *srx = dm->fun->acquire(dm, rt_rx.stream, rt_rx.channel, DMA_DIR_P2M);
    if (!stx || !srx) {                 /* one failed: undo the other */
        if (stx) dm->fun->free(dm, stx);
        if (srx) dm->fun->free(dm, srx);
        return -1;
    }
    p->dma_dev = dm; p->dma_tx = stx; p->dma_rx = srx;
    return 0;
}

static void spi_dma_release(spi *p)
{
    spi_hal_disable_tx_dma(p->hal);
    spi_hal_disable_rx_dma(p->hal);
    if (p->dma_dev) {
        if (p->dma_tx) p->dma_dev->fun->free(p->dma_dev, p->dma_tx);
        if (p->dma_rx) p->dma_dev->fun->free(p->dma_dev, p->dma_rx);
    }
    p->dma_dev = NULL; p->dma_tx = NULL; p->dma_rx = NULL;
}

/* Full-duplex DMA transfer: arm TX (from caller tx, bounced; or 0xFF dummy) and
 * RX (into caller rx, bounced; or a discard buffer) simultaneously. Wait for
 * both streams to hit Transfer-Complete. Returns 0 on success. */
static int spi_dma_xfer(spi *p, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (!p->dma_dev || !p->dma_tx || !p->dma_rx) return -1;
    if (len == 0) return 0;
    void *dr = spi_hal_get_dr_addr(p->hal);

    const uint8_t *tx_src = tx; uint8_t tx_inc = 1;
    if (tx) {
        if (len <= SPI_DMA_BOUNCE) { memcpy(p->dma_bounce, tx, len); tx_src = p->dma_bounce; }
        else tx_src = tx;                 /* oversize: assume main-SRAM buffer */
    } else {
        memset(p->dma_bounce, 0xFF, (len <= SPI_DMA_BOUNCE) ? len : SPI_DMA_BOUNCE);
        tx_src = p->dma_bounce; tx_inc = 0;  /* repeat 0xFF (read-only master) */
    }

    uint8_t *rx_dst = rx; uint8_t rx_inc = 1;
    if (rx) {
        if (len <= SPI_DMA_BOUNCE) rx_dst = p->dma_rx_bounce;  /* separate RX scratch */
        else rx_dst = rx;
    } else {
        rx_dst = p->dma_bounce; rx_inc = 0;  /* discard (RX DMA must drain to avoid OVR) */
    }

    p->dma_dev->fun->config(p->dma_dev, p->dma_tx, dr, (void *)tx_src, len,
                            DMA_DATA_8, 0, tx_inc, DMA_PRIO_MED);
    p->dma_dev->fun->config(p->dma_dev, p->dma_rx, dr, rx_dst, len,
                            DMA_DATA_8, 0, rx_inc, DMA_PRIO_MED);
    spi_hal_enable_rx_dma(p->hal);
    spi_hal_enable_tx_dma(p->hal);
    p->dma_dev->fun->start(p->dma_dev, p->dma_rx, NULL, NULL);
    p->dma_dev->fun->start(p->dma_dev, p->dma_tx, NULL, NULL);
    int rc = p->dma_dev->fun->wait_done(p->dma_dev, p->dma_tx, 2000);
    if (rc == 0) rc = p->dma_dev->fun->wait_done(p->dma_dev, p->dma_rx, 2000);
    spi_hal_disable_tx_dma(p->hal);
    spi_hal_disable_rx_dma(p->hal);
    if (rc == 0 && rx && rx_dst != rx) memcpy(rx, p->dma_rx_bounce, len);
    return rc == 0 ? 0 : -1;
}

static int spi_dev_open(device *self)
{
    spi *p = (spi *)self;

    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->sck_port, p->sck_pin, p->sck_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: SCK P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->sck_port, (int)p->sck_pin); return -2;
        }
        if (pm->fun->request(pm, p->miso_port, p->miso_pin, p->miso_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: MISO P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->miso_port, (int)p->miso_pin); return -2;
        }
        if (pm->fun->request(pm, p->mosi_port, p->mosi_pin, p->mosi_af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "spi", "[spi] %s: MOSI P%c%d CONFLICT\n", p->parent.parent.name,
                   'A' + (int)p->mosi_port, (int)p->mosi_pin); return -2;
        }
        pinmux_pin_cfg_t cfg = { .af = p->sck_af, .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };
        pm->fun->config(pm, p->sck_port, p->sck_pin, &cfg);
        cfg.af = p->miso_af; cfg.pupd = 1;
        pm->fun->config(pm, p->miso_port, p->miso_pin, &cfg);
        cfg.af = p->mosi_af; cfg.pupd = 0;
        pm->fun->config(pm, p->mosi_port, p->mosi_pin, &cfg);
    }

    spi_hal_enable_clock(p->hal);
    spi_hal_config(p->hal, p->pclk_hz, p->baud_hz);   /* SPE=1 */

    /* Reserve the hard-wired DMA streams for this SPI's TX/RX (if configured). */
    spi_dma_acquire(p);

    /* Register the SPI ISR through the platform-independent irq framework. */
    if (p->irq >= 0) {
        irq_manager_attach(p->irq, spi_isr, p);
        if (p->parent.mode == STREAM_MODE_IRQ)
            irq_manager_enable(p->irq, spi_isr, p);
    }
    return 0;
}

static int spi_dev_close(device *self)
{
    spi *p = (spi *)self;
    spi_dma_release(p);
    if (p->irq >= 0) {
        spi_hal_disable_rxne_irq(p->hal);
        irq_manager_disable(p->irq, spi_isr, p);
    }
    spi_hal_set_peripheral_enable(p->hal, 0);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* =========================================================================
 * Stream-class virtual implementations
 * ========================================================================= */

static int spi_stream_read(stream_device *self, void *buf, size_t len)
{
    return spi_do_xfer((spi *)self, NULL, (uint8_t *)buf, (uint16_t)len);
}

static int spi_stream_write(stream_device *self, const void *buf, size_t len)
{
    return spi_do_xfer((spi *)self, (const uint8_t *)buf, NULL, (uint16_t)len);
}

static int spi_stream_flush(stream_device *self)
{
    (void)self;
    return 0;
}
static int spi_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }
static int spi_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }

/* =========================================================================
 * Internal: full-duplex transfer in the current mode (POLL or IRQ)
 * ========================================================================= */
static int spi_do_xfer(spi *p, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (p->parent.mode == STREAM_MODE_DMA)
        return spi_dma_xfer(p, tx, rx, len);
    if (p->parent.mode == STREAM_MODE_IRQ) {
        /* --- Interrupt-driven transfer --- */
        if (len == 0) return 0;
        /* Set up transfer state for the ISR. */
        p->tx_buf  = tx;
        p->rx_buf  = rx;
        p->xfer_len = len;
        p->xfer_pos = 0;
        osal_sem_init(&p->xfer_done, 0);

        /* Prime: write the first byte to DR to start shifting. */
        spi_hal_write_dr(p->hal, tx ? tx[0] : (uint8_t)0xFF);
        p->xfer_pos = 1;

        /* Enable RXNEIE; the ISR handles remaining bytes. */
        spi_hal_enable_rxne_irq(p->hal);

        /* Wait for the ISR to complete the transfer. */
        osal_sem_wait(&p->xfer_done);

        spi_hal_disable_rxne_irq(p->hal);
        return 0;
    }
    /* Polling mode (default) */
    return spi_hal_transfer(p->hal, tx, rx, len);
}

/* =========================================================================
 * SPI ISR (RXNE-driven, full-duplex)
 *
 * Fires when a byte is received (RXNE=1). On entry:
 *   - DR holds the received byte (byte at position xfer_pos-1).
 *   - The previous byte was already written (by us in the ISR for byte 0..N-2,
 *     or by the prime write for byte 0).
 *
 * The ISR reads DR (the received byte), then writes the next byte (if any more).
 * The last byte received completes the transfer without a subsequent write.
 * ========================================================================= */
static void spi_isr(void *ctx)
{
    spi *p = (spi *)ctx;

    /* Read the received byte (this clears RXNE). */
    uint8_t rx_byte = spi_hal_read_dr(p->hal);
    uint16_t pos = p->xfer_pos;            /* index of the byte JUST received */

    if (pos > 0 && p->rx_buf)
        p->rx_buf[pos - 1] = rx_byte;

    /* If more bytes to send, write the next one. */
    if (pos < p->xfer_len) {
        spi_hal_write_dr(p->hal,
            p->tx_buf ? p->tx_buf[pos] : (uint8_t)0xFF);
        p->xfer_pos = pos + 1;
    } else {
        /* Last byte received — signal completion. */
        osal_sem_give(&p->xfer_done);
    }
}

/* =========================================================================
 * Base device vtable: forward to stream vtable, plus ioctl
 * ========================================================================= */
static int spi_dev_read(device *self, void *buf, size_t len)
    { return spi_stream_read((stream_device *)self, buf, len); }
static int spi_dev_write(device *self, const void *buf, size_t len)
    { return spi_stream_write((stream_device *)self, buf, len); }

static int spi_dev_ioctl(device *self, int cmd, void *arg)
{
    spi *p = (spi *)self;
    switch (cmd) {
    case SPI_IOCTL_XFER: {
        if (!arg) return -1;
        spi_xfer_t *x = (spi_xfer_t *)arg;
        return spi_do_xfer(p, x->tx_buf, x->rx_buf, x->len);
    }
    case SPI_IOCTL_GET_CR1:
        if (arg) *(uint32_t *)arg = spi_hal_get_cr1(p->hal);
        return 0;
    case SPI_IOCTL_GET_BSY:
        if (arg) *(int *)arg = spi_hal_is_busy(p->hal);
        return 0;
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        if (m == STREAM_MODE_DMA) {
            /* need both streams reserved for full-duplex SPI DMA */
            if (!p->dma_tx || !p->dma_rx) return -1;
            /* silence the SPI's own data IRQ so the ISR can't steal a byte */
            spi_hal_disable_rxne_irq(p->hal);
            if (p->irq >= 0) irq_manager_disable(p->irq, spi_isr, p);
        } else if (m == STREAM_MODE_IRQ) {
            if (p->irq >= 0) {
                spi_hal_enable_rxne_irq(p->hal);
                irq_manager_enable(p->irq, spi_isr, p);
            }
        } else if (m == STREAM_MODE_POLL) {
            spi_hal_disable_rxne_irq(p->hal);
            if (p->irq >= 0) irq_manager_disable(p->irq, spi_isr, p);
        } else {
            return -1;
        }
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
