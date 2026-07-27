#include "dac.h"
#include "devmgr/device_manager.h"
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */
#include "log/log.h"
#include "log/app_log.h"

/* virtual implementations dispatched through the unified device vtable */
static int dac_dev_open(device *self);
static int dac_dev_close(device *self);
static int dac_dma_acquire(dac *p);
static void dac_dma_release(dac *p);
static int dac_setup_engine(dac *p, stream_xfer_mode_t engine);
static void dac_free_engine(dac *p);
static int dac_dma_write(dac *p, const void *buf, size_t len);
static int dac_dev_read(device *self, void *buf, size_t len);
static int dac_dev_write(device *self, const void *buf, size_t len);
static int dac_dev_ioctl(device *self, int cmd, void *arg);

/* stream-class function forward declarations (referenced by the vtable below) */
static int dac_stream_read(stream_device *self, void *buf, size_t len);
static int dac_stream_write(stream_device *self, const void *buf, size_t len);
static int dac_stream_flush(stream_device *self);
static int dac_sr(stream_device *self, void *b, size_t l, void *m);
static int dac_sw(stream_device *self, const void *b, size_t l, const void *m);

static const struct stream_deviceVtable dac_stream_vtable = {
    .read        = dac_stream_read,
    .write       = dac_stream_write,
    .flush       = dac_stream_flush,
    .read_frame  = dac_sr,
    .write_frame = dac_sw,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

static const struct deviceVtable dac_dev_vtable = {
    .open  = dac_dev_open,
    .close = dac_dev_close,
    .read  = dac_dev_read,
    .write = dac_dev_write,
    .ioctl = dac_dev_ioctl,
};

device *dac_create(const void *config)
{
    const dac_config_t *c = (const dac_config_t *)config;
    if (!c) return NULL;
    dac *p = (dac *)malloc(sizeof(dac));
    if (!p) return NULL;
    memset(p, 0, sizeof(dac));
    p->hal = dac_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->channel = c->channel;
    p->out_signal = c->out_signal;
    p->dma_req = c->dma_req;              /* cached for re-acquire on reopen */
    p->dma_dev = NULL; p->dma_str = NULL;
    p->trig_tim = c->trig_tim;            /* cached trigger timer (TIM6) for DAC+DMA */
    /* Resolve the output signal name up front. */
    if (c->out_signal)
        pinmux_hal_resolve(c->out_signal, &p->port, &p->pin, &p->af);
    p->parent.parent.type   = DEVICE_TYPE_DAC;     /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &dac_dev_vtable;     /* base device vtable */
    p->parent.vtable        = &dac_stream_vtable;  /* stream-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_STREAM;
    p->parent.mode          = STREAM_MODE_POLL;    /* DAC writes are synchronous */
    return (device *)p;
}

void dac_destroy(dac *self)
{
    if (!self) return;
    dac_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int dac_dev_open(device *self)
{
    dac *p = (dac *)self;
    /* Claim + program the analog output pin through the pinmux BEFORE touching
     * the GPIO registers. A conflict makes the request fail and we refuse. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm && p->out_signal) {
        if (pm->fun->request(pm, p->port, p->pin, p->af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "dac", "%s: pin P%c%d CONFLICT — refused",
                   p->parent.parent.name, 'A' + p->port, p->pin);
            return -2;
        }
        pinmux_pin_cfg_t cfg = {
            .af = p->af, .mode = 3, .otype = 0, .speed = 0, .pupd = 0
        };
        pm->fun->config(pm, p->port, p->pin, &cfg);   /* mode=3 -> analog */
    }
    dac_hal_enable_clock(p->hal);
    dac_hal_enable_channel(p->hal, p->channel);
    dac_dma_acquire(p);    /* reserve the hard-wired DMA stream (if any) */
    /* (Re)build the per-engine state for the chosen engine (default POLL) and
     * select the trigger (DMA only). If DMA is unavailable, fall back to POLL so
     * the dac stays usable. */
    if (dac_setup_engine(p, p->parent.mode) != 0)
        dac_setup_engine(p, STREAM_MODE_POLL);
    return 0;
}

static int dac_dev_close(device *self)
{
    dac *p = (dac *)self;
    dac_free_engine(p);    /* free per-engine state (idempotent; destroys trig) */
    dac_dma_release(p);    /* release the reserved DMA stream */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

/* --- DMA engine (STREAM_MODE_DMA, M2P: memory -> DAC_DHR) ---
 * A DAC channel is hard-wired to ONE specific DMA stream (DAC1->DMA1_Stream5).
 * We resolve that stream once at open() (via dma_hal_route) and keep it reserved;
 * the DMA ISR (in drv/dma.c) gives the stream's done_sem on Transfer-Complete,
 * so the write blocks until the whole burst has been streamed into the DAC. */
static int dac_dma_acquire(dac *p)
{
    p->dma_dev = NULL; p->dma_str = NULL;
    /* Reserve the hard-wired DMA stream (DAC1->DMA1_Stream5). The trigger timer
     * requirement is enforced later, in dac_setup_engine(DMA) — DAC+DMA needs a
     * timer TRGO because static mode never raises a DMA request. */
    if (p->dma_req == DMA_REQ_NONE) return 0;
    dma_route_t rt = dma_hal_route(p->dma_req);
    if (!rt.name) return 0;
    dma *dm = (dma *)device_manager_get(rt.name);
    if (!dm) return 0;
    dma_stream_t *s = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_M2P);
    if (s) { p->dma_dev = dm; p->dma_str = s; }
    else log_printf(app_log(), LOG_DEBUG, "dac", "%s: DMA stream busy\n",
                   p->parent.parent.name);
    return (p->dma_str) ? 0 : -1;
}

/* Lazily create + configure the DAC trigger timer (TIM6) for DAC+DMA. Returns 0
 * if a trigger is available (handle created/kept in e->trig_hal), -1 if the
 * board supplied no trigger timer (DAC+DMA is then impossible). */
static int dac_ensure_trigger(dac *p, dac_dma_t *e)
{
    if (!p->trig_tim) { e->has_trigger = 0; return -1; }
    if (!e->trig_hal) {
        e->trig_hal = tim_hal_create(p->trig_tim);
        if (!e->trig_hal) { e->has_trigger = 0; return -1; }
        tim_hal_enable_clock(e->trig_hal);
        /* ~1 MHz trigger: plenty fast for a short burst, and any rate works
         * since the DAC DMA blocks on Transfer-Complete. Route Update->TRGO. */
        tim_hal_config(e->trig_hal, 84000000U, 1000000U);
        tim_hal_master_trgo_update(e->trig_hal);
    }
    e->has_trigger = 1;
    return 0;
}

/* --- Per-engine state management (lazy allocation, mirrors uart_setup_engine) ---
 * The engine is POLL/DMA; only the DMA engine needs state (dac_dma_t: the sample
 * bounce + the trigger timer). Reached via a single `p->eng` cast. Returns 0 on
 * success, -1 if DMA is unavailable (leaving p untouched). */
static void dac_free_engine(dac *p)
{
    if (!p->eng) return;
    dac_dma_t *e = (dac_dma_t *)p->eng;
    if (e->trig_hal) { tim_hal_destroy(e->trig_hal); e->trig_hal = NULL; }
    free(p->eng);
    p->eng = NULL;
}

static int dac_setup_engine(dac *p, stream_xfer_mode_t engine)
{
    if (engine != STREAM_MODE_POLL && engine != STREAM_MODE_DMA)
        return -1;
    /* DMA needs the hard-wired stream reserved at open(). */
    if (engine == STREAM_MODE_DMA && !p->dma_str) return -1;

    dac_free_engine(p);   /* teardown (after validation) */

    if (engine == STREAM_MODE_DMA) {
        dac_dma_t *e = (dac_dma_t *)malloc(sizeof(dac_dma_t));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        p->eng = e;
        /* DAC+DMA REQUIRES a trigger timer (static mode never raises a DMA
         * request). If none is available, reject the mode cleanly. */
        if (dac_ensure_trigger(p, e) != 0) {
            dac_free_engine(p);
            return -1;
        }
        dac_hal_enable_trigger(p->hal, p->channel, 0 /*TIM6 TRGO*/);
    } else { /* POLL: static mode, no engine state */
        p->eng = NULL;
        dac_hal_disable_trigger(p->hal, p->channel);
    }
    p->parent.mode = engine;
    return 0;
}

/* Release the reserved DMA stream (called at close). */
static void dac_dma_release(dac *p)
{
    if (p->dma_dev && p->dma_str) p->dma_dev->fun->free(p->dma_dev, p->dma_str);
    p->dma_dev = NULL; p->dma_str = NULL;
}

/* DMA burst write: copy the caller's 12-bit samples into the main-SRAM bounce
 * (DMA cannot touch CCM), program the reserved stream (M2P, PAR=DHR12Rx, memory=
 * bounce, MINC), and CLOCK the burst with the trigger timer (TIM6 TRGO). Each
 * timer overflow moves DHR->DOR and raises the DAC's DMA request, so the stream
 * streams N samples without any CPU/ISR involvement. After Transfer-Complete we
 * stop the timer and issue one software trigger to push the final held sample
 * into DOR, which the BIST reads back to prove the path. */
static int dac_dma_write(dac *p, const void *buf, size_t len)
{
    dac_dma_t *e = (dac_dma_t *)p->eng;
    if (!e || !p->dma_dev || !p->dma_str || !e->trig_hal) return -1;
    uint32_t nsamp = (uint32_t)(len / sizeof(uint16_t));
    if (nsamp == 0) return -1;
    const uint16_t *src = (const uint16_t *)buf;
    uint16_t *tmp = (nsamp <= DAC_DMA_BOUNCE) ? e->dma_bounce
                                              : (uint16_t *)malloc(nsamp * sizeof(uint16_t));
    if (!tmp) return -1;
    for (uint32_t i = 0; i < nsamp; i++) tmp[i] = src[i] & 0x0FFFU;
    void *dhr = dac_hal_get_dhr_addr(p->hal, p->channel);
    p->dma_dev->fun->config(p->dma_dev, p->dma_str, dhr, tmp, nsamp,
                            DMA_DATA_16, 0 /*periph_inc*/, 1 /*mem_inc*/, DMA_PRIO_MED);
    p->dma_dev->fun->start(p->dma_dev, p->dma_str, NULL, NULL);  /* arm stream (EN=1) */
    dac_hal_enable_dma(p->hal, p->channel);    /* DAC now raises DMA requests */
    tim_hal_start(e->trig_hal);                /* timer TRGO clocks DHR->DOR + DMA req */
    int rc = p->dma_dev->fun->wait_done(p->dma_dev, p->dma_str, 2000);
    tim_hal_stop(e->trig_hal);                 /* freeze the trigger source */
    /* Flush the final held sample. In trigger mode the last DMA write sits in
     * DHR awaiting one more trigger edge; switching back to static mode (TENx=0)
     * makes the held value auto-transfer to DOR, and we re-write it to be
     * certain. (A software trigger alone did not reliably move it here.) */
    dac_hal_disable_trigger(p->hal, p->channel);
    dac_hal_write(p->hal, p->channel, tmp[nsamp - 1] & 0x0FFFU);
    dac_hal_disable_dma(p->hal, p->channel);
    if (tmp != e->dma_bounce) free(tmp);
    return rc == 0 ? (int)(nsamp * sizeof(uint16_t)) : -1;
}

/* stream-class ops. A DAC is an output stream: one write() outputs one 12-bit
 * sample. The transfer engine is POLL only (DAC writes are synchronous, no IRQ
 * needed); read()/read_frame() are unsupported (use GET_VALUE ioctl to read DOR). */
static int dac_stream_read(stream_device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }   /* DAC is write-only stream */
static int dac_stream_write(stream_device *self, const void *buf, size_t len)
{
    dac *p = (dac *)self;
    if (len < sizeof(uint16_t)) return -1;
    if (p->parent.mode == STREAM_MODE_DMA) return dac_dma_write(p, buf, len);
    dac_hal_write(p->hal, p->channel, *(const uint16_t *)buf);
    return (int)sizeof(uint16_t);
}
static int dac_stream_flush(stream_device *self) { (void)self; return 0; }
static int dac_sr(stream_device *self, void *b, size_t l, void *m)
    { (void)self; (void)b; (void)l; (void)m; return -1; }
static int dac_sw(stream_device *self, const void *b, size_t l, const void *m)
    { (void)self; (void)b; (void)l; (void)m; return -1; }

/* base device-interface ops forward to the stream-class vtable */
static int dac_dev_read(device *self, void *buf, size_t len)
    { return dac_stream_read((stream_device *)self, buf, len); }
static int dac_dev_write(device *self, const void *buf, size_t len)
    { return dac_stream_write((stream_device *)self, buf, len); }

static int dac_dev_ioctl(device *self, int cmd, void *arg)
{
    dac *p = (dac *)self;
    switch (cmd) {
    case DAC_IOCTL_SET_VALUE: {
        if (!arg) return -1;
        dac_hal_write(p->hal, p->channel, *(const uint16_t *)arg);
        return 0;
    }
    case DAC_IOCTL_GET_VALUE: {
        if (!arg) return -1;
        *(uint16_t *)arg = dac_hal_get_dor(p->hal, p->channel);
        return 0;
    }
    case DAC_IOCTL_GET_CR: {
        if (!arg) return -1;
        *(uint32_t *)arg = dac_hal_get_cr(p->hal);
        return 0;
    }
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        /* (re)build per-engine state for the new engine (frees the old, allocates
         * the new, enables/ disables the trigger). Returns -1 (leaving the dac in
         * its previous configuration) if the engine is unavailable. */
        return dac_setup_engine(p, m);
    }
    case STREAM_IOCTL_GET_MODE: {
        if (!arg) return -1;
        *(stream_xfer_mode_t *)arg = p->parent.mode;
        return 0;
    }
    default:
        return -1;
    }
}
