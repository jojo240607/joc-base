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
static int  sdio_setup_engine(sdio *p, stream_xfer_mode_t engine);
static void sdio_free_engine(sdio *p);
static int  sdio_dma_acquire(sdio *p);
static void sdio_dma_release(sdio *p);
static int  sdio_dma_xfer(sdio *p, sdio_cmd_data_t *x);

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
    p->dma_req = c->dma_req;
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

/* --- DMA engine (STREAM_MODE_DMA) ---
 * Reserve the hard-wired SDIO DMA stream (via dma_hal_route) on demand. The
 * SDIO host has ONE DMA request, so a single stream serves both read (P2M) and
 * write (M2P); its direction is reconfigured per transfer. The DMA ISR (in
 * drv/dma.c) gives the stream done_sem on TC/TE. */
static int sdio_dma_acquire(sdio *p)
{
    if (p->dma_req == DMA_REQ_NONE) return -1;
    dma_route_t rt = dma_hal_route(p->dma_req);
    if (!rt.name) return -1;
    dma *dm = (dma *)device_manager_get(rt.name);
    if (!dm) return -1;
    dma_stream_t *s = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_M2P);
    if (!s) return -1;
    sdio_dma_t *e = (sdio_dma_t *)malloc(sizeof(sdio_dma_t));
    if (!e) { dm->fun->free(dm, s); return -1; }
    memset(e, 0, sizeof(*e));
    e->dma_dev = dm; e->dma_s = s;
    p->eng = e;
    return 0;
}

static void sdio_dma_release(sdio *p)
{
    sdio_dma_t *e = (sdio_dma_t *)p->eng;
    if (e) {
        if (e->dma_dev && e->dma_s) {
            sdio_hal_dma_enable(p->hal, 0);
            e->dma_dev->fun->free(e->dma_dev, e->dma_s);
        }
        free(e);
    }
    p->eng = NULL;
}

/* --- Per-engine state management (lazy allocation, mirrors uart/i2s) ---
 * POLL needs nothing; DMA needs the reserved stream (sdio_dma_t). Returns 0 on
 * success, -1 if DMA is unavailable (leaving p untouched). */
static void sdio_free_engine(sdio *p)
{
    if (!p->eng) return;
    sdio_dma_release(p);   /* frees the per-engine struct, clears p->eng */
}

static int sdio_setup_engine(sdio *p, stream_xfer_mode_t engine)
{
    if (engine != STREAM_MODE_POLL && engine != STREAM_MODE_DMA)
        return -1;
    if (engine == STREAM_MODE_DMA) {
        if (sdio_dma_acquire(p) != 0) return -1;   /* refused: leave p as-is */
    } else {
        sdio_free_engine(p);
    }
    p->parent.mode = engine;
    return 0;
}

/* DMA data block transfer (read = card→host P2M, write = host→card M2P). The
 * caller buffer MUST be in main SRAM and a multiple of 4 bytes (SD blocks are
 * 512 B). The START/command happen on the CPU; the byte movement is offloaded to
 * the DMA controller, which the SDIO host paces from its FIFO. Returns 0 on
 * success, -1 on error/timeout. */
static int sdio_dma_xfer(sdio *p, sdio_cmd_data_t *x)
{
    sdio_dma_t *e = (sdio_dma_t *)p->eng;
    if (!e || !e->dma_dev || !e->dma_s || !x->buf) return -1;
    uint32_t blk = x->blk_size ? x->blk_size : 512U;
    uint32_t cnt = x->blk_count ? x->blk_count : 1U;
    uint32_t total = blk * cnt;
    if (total == 0 || (total & 3U)) return -1;          /* word-aligned only */
    int is_write = (x->data_dir != 0U);                 /* 0=read(P2M),1=write(M2P) */

    /* Configure the one SDIO stream for this transfer's direction. */
    void *fifo = sdio_hal_get_fifo_addr(p->hal);
    e->dma_dev->fun->config(e->dma_dev, e->dma_s, fifo, x->buf, total / 4U,
                            DMA_DATA_32, 0 /*PINC*/, 1 /*MINC*/, DMA_PRIO_HIGH);

    /* Data path: DTEN + DMAEN, direction from DTDIR (matches `data_dir`). */
    sdio_hal_data_config_dma(p->hal, x->data_dir, blk, cnt);

    /* Command that starts the data phase (caller already supplies a block addr). */
    uint32_t cmd = (cnt == 1U) ? (is_write ? 24U : 17U) : (is_write ? 25U : 18U);
    if (sdio_hal_cmd(p->hal, cmd, x->arg, 1, NULL)) {
        sdio_hal_dma_enable(p->hal, 0); sdio_hal_data_enable(p->hal, 0);
        return -1;
    }

    /* Arm the DMA AFTER the command so a write can never overfill the FIFO
     * before the SDIO data phase begins. */
    e->dma_dev->fun->start(e->dma_dev, e->dma_s, NULL, NULL);

    if (sdio_hal_wait_data_end(p->hal, 5000000U) != 0) {
        e->dma_dev->fun->stop(e->dma_dev, e->dma_s);
        sdio_hal_dma_enable(p->hal, 0); sdio_hal_data_enable(p->hal, 0);
        return -1;
    }
    int rc = e->dma_dev->fun->wait_done(e->dma_dev, e->dma_s, 2000);
    e->dma_dev->fun->stop(e->dma_dev, e->dma_s);
    sdio_hal_dma_enable(p->hal, 0); sdio_hal_data_enable(p->hal, 0);
    sdio_hal_clear_data_icr(p->hal);
    if (cnt > 1U) sdio_hal_stop_transfer(p->hal);
    return rc ? -1 : 0;
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
    /* Build per-engine state for the default (POLL) engine. A POLL SDIO pays
     * nothing; switching to DMA later (SET_MODE) allocates the per-engine
     * stream state on demand. */
    sdio_setup_engine(p, STREAM_MODE_POLL);
    return 0;
}
static int sdio_dev_close(device *self)
{
    sdio *p = (sdio *)self;
    sdio_free_engine(p);          /* free per-engine state (idempotent) */
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
    case SDIO_IOCTL_CMD_DATA: {
        sdio_cmd_data_t *x = arg; if (!x) return -1;
        if (p->parent.mode == STREAM_MODE_DMA)
            x->result = sdio_dma_xfer(p, x);
        else if (x->data_dir == 0)  /* read: card → host */
            x->result = sdio_hal_read_block(p->hal, x->buf, x->arg, x->blk_count, 1);
        else                    /* write: host → card */
            x->result = sdio_hal_write_block(p->hal, x->buf, x->arg, x->blk_count, 1);
        return x->result;
    }
    case SDIO_IOCTL_SET_CLOCK: { if (!arg) return -1; sdio_hal_set_clock_div(p->hal, *(uint32_t*)arg); return 0; }
    case SDIO_IOCTL_GET_POWER: if (arg) *(uint32_t*)arg = sdio_hal_get_power(p->hal); return 0;
    case SDIO_IOCTL_GET_CLKCR: if (arg) *(uint32_t*)arg = sdio_hal_get_clkcr(p->hal); return 0;
    case STREAM_IOCTL_SET_MODE: { if (!arg) return -1; return sdio_setup_engine(p, *(const stream_xfer_mode_t*)arg); }
    case STREAM_IOCTL_GET_MODE: { if (arg) *(stream_xfer_mode_t*)arg = p->parent.mode; return 0; }
    default: return -1;
    }
}
