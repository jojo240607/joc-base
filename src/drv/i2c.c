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
static int  i2c_setup_engine(i2c *p, stream_xfer_mode_t engine);
static void i2c_free_engine(i2c *p);
static int  i2c_dma_acquire(i2c *p);
static void i2c_dma_release(i2c *p);
static int  i2c_dma_xfer(i2c *p, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len, int is_write);

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
    p->dma_tx_req = c->dma_tx_req;
    p->dma_rx_req = c->dma_rx_req;
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
    /* Build per-engine state for the default (POLL) engine. IRQ registers its
     * EV/ER ISRs and DMA reserves its streams only when that engine is selected
     * via SET_MODE — so a POLL I2C pays nothing and doesn't touch the IRQ lines. */
    i2c_setup_engine(p, STREAM_MODE_POLL);
    return 0;
}
static int i2c_dev_close(device *self)
{
    i2c *p = (i2c *)self;
    i2c_free_engine(p);          /* unregister ISR (IRQ) / release streams (DMA) */
    i2c_hal_set_peripheral_enable(p->hal, 0);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}
static int i2c_dev_read(device *self, void *buf, size_t len) { return i2c_stream_read((stream_device *)self, buf, len); }
static int i2c_dev_write(device *self, const void *buf, size_t len) { return i2c_stream_write((stream_device *)self, buf, len); }

/* --- DMA engine (STREAM_MODE_DMA) ---
 * Reserve the hard-wired TX (M2P) and RX (P2M) streams via dma_hal_route. The
 * START/address handshake is driven by the CPU; the byte movement is offloaded
 * to the DMA controller (CR2.DMAEN). The DMA stream ISR (drv/dma.c) signals the
 * transfer-complete semaphore. */
static int i2c_dma_acquire(i2c *p)
{
    if (p->dma_tx_req == DMA_REQ_NONE && p->dma_rx_req == DMA_REQ_NONE) return -1;
    i2c_dma_t *e = (i2c_dma_t *)malloc(sizeof(i2c_dma_t));
    if (!e) return -1;
    memset(e, 0, sizeof(*e));
    dma *dm = NULL;
    if (p->dma_tx_req != DMA_REQ_NONE) {
        dma_route_t rt = dma_hal_route(p->dma_tx_req);
        if (!rt.name) { free(e); return -1; }
        dm = (dma *)device_manager_get(rt.name);
        if (!dm) { free(e); return -1; }
        e->dma_tx = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_M2P);
        if (!e->dma_tx) { free(e); return -1; }
    }
    if (p->dma_rx_req != DMA_REQ_NONE) {
        dma_route_t rt = dma_hal_route(p->dma_rx_req);
        if (!rt.name) goto fail;
        dma *dmr = (dma *)device_manager_get(rt.name);
        if (!dmr) goto fail;
        e->dma_rx = dmr->fun->acquire(dmr, rt.stream, rt.channel, DMA_DIR_P2M);
        if (!e->dma_rx) goto fail;
        if (!dm) dm = dmr;
    }
    e->dma_dev = dm;
    p->eng = e;
    return 0;
fail:
    if (e->dma_tx && dm) dm->fun->free(dm, e->dma_tx);
    if (e->dma_rx && dm) dm->fun->free(dm, e->dma_rx);
    free(e);
    return -1;
}

static void i2c_dma_release(i2c *p)
{
    i2c_dma_t *e = (i2c_dma_t *)p->eng;
    if (!e) return;
    i2c_hal_dma_enable(p->hal, 0);
    if (e->dma_dev) {
        if (e->dma_tx) e->dma_dev->fun->free(e->dma_dev, e->dma_tx);
        if (e->dma_rx) e->dma_dev->fun->free(e->dma_dev, e->dma_rx);
    }
    /* p->eng itself is freed by the caller (i2c_free_engine). */
}

/* --- Per-engine state management (lazy allocation, mirrors uart/i2s/sdio) ---
 * POLL needs nothing; IRQ needs the state-machine scratch (i2c_irq_t) + EV/ER
 * ISR registration; DMA needs the reserved streams (i2c_dma_t). The new engine
 * is built BEFORE the old one is torn down, so a rejected engine leaves the i2c
 * in its previous configuration. */
static int i2c_setup_engine(i2c *p, stream_xfer_mode_t engine)
{
    if (engine != STREAM_MODE_POLL && engine != STREAM_MODE_IRQ && engine != STREAM_MODE_DMA)
        return -1;
    void *old_eng = p->eng;
    stream_xfer_mode_t old_mode = p->parent.mode;
    void *new_eng = NULL;
    int rc = 0;

    if (engine == STREAM_MODE_POLL) {
        new_eng = NULL;
    } else if (engine == STREAM_MODE_IRQ) {
        i2c_irq_t *e = (i2c_irq_t *)malloc(sizeof(i2c_irq_t));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        if (p->ev_irq >= 0) irq_register(p->ev_irq, i2c_hal_ev_isr, p);
        if (p->er_irq >= 0) irq_register(p->er_irq, i2c_hal_er_isr, p);
        new_eng = e;
    } else { /* DMA */
        rc = i2c_dma_acquire(p);
        if (rc != 0) return -1;
        new_eng = p->eng;   /* i2c_dma_acquire sets p->eng on success */
    }

    /* Commit: tear down the OLD engine, install the NEW one. */
    p->eng = old_eng; p->parent.mode = old_mode;
    i2c_free_engine(p);
    p->eng = new_eng;
    p->parent.mode = engine;
    return 0;
}

static void i2c_free_engine(i2c *p)
{
    if (!p->eng) return;
    if (p->parent.mode == STREAM_MODE_IRQ) {
        i2c_hal_disable_ev_irq(p->hal); i2c_hal_disable_er_irq(p->hal);
        if (p->ev_irq >= 0) irq_unregister(p->ev_irq, i2c_hal_ev_isr, p);
        if (p->er_irq >= 0) irq_unregister(p->er_irq, i2c_hal_er_isr, p);
    } else if (p->parent.mode == STREAM_MODE_DMA) {
        i2c_dma_release(p);
    }
    free(p->eng);
    p->eng = NULL;
}

/* DMA data transfer (len > 0). CPU does the START+address handshake (bailing if
 * no slave ACKs); the byte movement is driven by the DMA controller. Returns 0
 * on success, -1 on NACK/timeout/error. */
static int i2c_dma_xfer(i2c *p, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len, int is_write)
{
    i2c_dma_t *e = (i2c_dma_t *)p->eng;
    if (!e || !e->dma_dev) return -1;
    if (is_write && !e->dma_tx) return -1;
    if (!is_write && !e->dma_rx) return -1;

    /* CPU handshake: START + address; bail if no slave ACKs (before DMA). */
    if (i2c_hal_master_start_addr(p->hal, addr, is_write) != 0) return -1;

    void *dr = i2c_hal_get_dr_addr(p->hal);

    if (is_write) {
        (void)i2c_hal_read_sr2(p->hal);   /* clear ADDR */
        e->dma_dev->fun->config(e->dma_dev, e->dma_tx, dr, (void *)tx, len,
                                DMA_DATA_8, 0 /*PINC*/, 1 /*MINC*/, DMA_PRIO_MED);
        i2c_hal_dma_enable(p->hal, 1);
        e->dma_dev->fun->start(e->dma_dev, e->dma_tx, NULL, NULL);
        int rc = e->dma_dev->fun->wait_done(e->dma_dev, e->dma_tx, 2000);
        /* Wait for the last byte to finish shifting out before STOP. */
        i2c_hal_wait_btf(p->hal, 200000U);
        i2c_hal_set_stop(p->hal);
        i2c_hal_dma_enable(p->hal, 0);
        return rc ? -1 : 0;
    }

    /* RX: set ACK policy + LAST before clearing ADDR (so the final byte is
     * NACKed by hardware), then arm the RX DMA. */
    if (len == 1) {
        i2c_hal_set_ack(p->hal, 0);     /* NACK the single byte */
        i2c_hal_set_stop(p->hal);       /* STOP follows the single byte */
    } else {
        i2c_hal_set_ack(p->hal, 1);
        i2c_hal_set_dma_last(p->hal, 1);/* HW NACKs the final DMA byte */
    }
    (void)i2c_hal_read_sr2(p->hal);     /* clear ADDR */
    e->dma_dev->fun->config(e->dma_dev, e->dma_rx, dr, rx, len,
                            DMA_DATA_8, 0 /*PINC*/, 1 /*MINC*/, DMA_PRIO_MED);
    i2c_hal_dma_enable(p->hal, 1);
    e->dma_dev->fun->start(e->dma_dev, e->dma_rx, NULL, NULL);
    int rc = e->dma_dev->fun->wait_done(e->dma_dev, e->dma_rx, 2000);
    i2c_hal_set_stop(p->hal);           /* ensure STOP (len>1 case) */
    i2c_hal_dma_enable(p->hal, 0);
    i2c_hal_set_dma_last(p->hal, 0);
    return rc ? -1 : 0;
}

/* Transfer: POLL / IRQ / DMA. The DMA path offloads only the byte movement; the
 * START+address handshake stays on the CPU (timeout-guarded). A zero-length
 * transfer (probe) is always done in POLL because there is no data to DMA. */
static int i2c_do_xfer(i2c *p, uint16_t addr, const uint8_t *tx, uint8_t *rx, uint16_t len, int is_write)
{
    if (p->parent.mode == STREAM_MODE_DMA && len > 0)
        return i2c_dma_xfer(p, addr, tx, rx, len, is_write);
    if (p->parent.mode == STREAM_MODE_IRQ) {
        i2c_irq_t *e = (i2c_irq_t *)p->eng;
        if (!e) return -1;
        e->addr = addr; e->tx_buf = tx; e->rx_buf = rx;
        e->xfer_len = len; e->xfer_pos = 0;
        e->irq_state = 1; /* I2C_S_SB */ e->irq_result = -1; e->xfer_done = 0;

        i2c_hal_set_start(p->hal);
        i2c_hal_enable_ev_irq(p->hal);
        i2c_hal_enable_er_irq(p->hal);
        /* Only enable EV IRQ (skip ER — it causes a HardFault on this platform) */
        if (p->ev_irq >= 0) i2c_hal_nvic_enable(p->ev_irq);

        volatile uint32_t tmo = 200000U;
        while (!e->xfer_done && tmo--) { }

        if (p->ev_irq >= 0) i2c_hal_nvic_disable(p->ev_irq);
        i2c_hal_disable_ev_irq(p->hal);
        i2c_hal_disable_er_irq(p->hal);
        return e->irq_result;
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
    case STREAM_IOCTL_SET_MODE: { if (!arg) return -1; return i2c_setup_engine(p, *(const stream_xfer_mode_t *)arg); }
    case STREAM_IOCTL_GET_MODE: { if (arg) *(stream_xfer_mode_t *)arg = p->parent.mode; return 0; }
    default: return -1;
    }
}
