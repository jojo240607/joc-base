#include "dma.h"
#include "dma_hal.h"
#include "irq_manager.h"              /* centralized interrupt manager */
#include <stdlib.h>
#include <string.h>
#include "log/log.h"
#include "log/app_log.h"

/* --- private per-stream runtime state lives in dma.h (dma_stream_rt_t) --- */

/* virtual implementations dispatched through the unified device vtable */
static int dma_dev_open(device *self);
static int dma_dev_close(device *self);
static int dma_dev_read(device *self, void *buf, size_t len);
static int dma_dev_write(device *self, const void *buf, size_t len);
static int dma_dev_ioctl(device *self, int cmd, void *arg);

/* typed convenience methods — reachable ONLY through self->fun->xxx(self, ...),
 * never as standalone functions (the concrete impls are `static` in this file) */
static dma_stream_t *dma_acquire(dma *self, uint8_t stream_idx, uint8_t channel, dma_dir_t dir);
static int dma_stream_config(dma *self, dma_stream_t *s, const void *periph,
                             void *mem, uint32_t count, dma_data_size_t size,
                             int periph_inc, int mem_inc, dma_prio_t prio);
static int dma_stream_start(dma *self, dma_stream_t *s, void (*cb)(void *), void *ctx);
static int dma_wait_done(dma *self, dma_stream_t *s, uint32_t timeout_ms);
static int dma_poll_done(dma *self, dma_stream_t *s);
static uint32_t dma_remaining(dma *self, dma_stream_t *s);
static void dma_stream_free(dma *self, dma_stream_t *s);

/* control-class ops */
static int dma_control_command(control_device *self, int cmd, void *arg);
static int dma_control_set(control_device *self, int param, const void *val);
static int dma_control_get(control_device *self, int param, void *val);

/* internal */
static void dma_isr(void *ctx);   /* per-stream TC/TE ISR (via irq framework) */

const struct dmaFun dma_fun = {
    .destroy     = dma_destroy,
    .init        = dma_init,
    .deinit      = dma_deinit,
    .acquire     = dma_acquire,
    .config      = dma_stream_config,
    .start       = dma_stream_start,
    .wait_done   = dma_wait_done,
    .poll_done   = dma_poll_done,
    .remaining   = dma_remaining,
    .free        = dma_stream_free,
};

/* one shared vtable for the whole DMA class — assigned by dma_init() */
static const struct deviceVtable dma_dev_vtable = {
    .open  = dma_dev_open,
    .close = dma_dev_close,
    .read  = dma_dev_read,
    .write = dma_dev_write,
    .ioctl = dma_dev_ioctl,
};

static const struct control_deviceVtable dma_control_vtable = {
    .command = dma_control_command,
    .set     = dma_control_set,
    .get     = dma_control_get,
};

/* Uniform create signature for the board layer. */
device *dma_create(const void *config)
{
    const dma_config_t *c = (const dma_config_t *)config;
    dma *self = (dma *)malloc(sizeof(dma));
    if (!self) return NULL;
    memset(self, 0, sizeof(dma));

    self->parent.parent.type = DEVICE_TYPE_DMA;     /* driver sets its own class */
    self->parent.parent.name = c->name;             /* driver sets its own name */
    dma_init(self);

    /* bring up the controller + all 8 stream handles, and register a TC/TE ISR
     * (through the platform-independent irq framework) for each stream. The ISR
     * is lightweight and only fires when a transfer armed TCIE/TEIE. */
    dma_hal_enable_clock(c->periph);
    for (uint32_t i = 0; i < DMA_STREAMS_PER_CTLR; i++) {
        self->hal[i] = dma_hal_stream_create(c->periph, i);
        if (!self->hal[i]) { dma_destroy(self); return NULL; }
        self->streams[i].in_use  = 0;
        self->streams[i].hal     = self->hal[i];
        self->streams[i].owner   = self;
        self->handles[i].idx     = (int)i;
        osal_sem_init(&self->streams[i].done_sem, 0);
        irq_id_t id = dma_hal_stream_irq_id(self->hal[i]);
        irq_manager_set_priority(id, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        irq_manager_attach(id, dma_isr, &self->streams[i]);
        irq_manager_enable(id, dma_isr, &self->streams[i]);  /* arm NVIC (cb attached) */
    }
    return (device *)self;
}

void dma_destroy(dma *self)
{
    if (!self) return;
    dma_deinit(self);
    for (uint32_t i = 0; i < DMA_STREAMS_PER_CTLR; i++) {
        if (self->hal[i]) { dma_hal_stream_destroy(self->hal[i]); self->hal[i] = NULL; }
    }
    free(self);
}

void dma_init(dma *self)
{
    if (!self) return;
    self->parent.parent.vtable = &dma_dev_vtable;       /* base device vtable */
    self->parent.vtable        = &dma_control_vtable;   /* control-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_DMA;
    self->parent.parent.class  = DEVICE_CLASS_CONTROL;
    self->fun = &dma_fun;
    /* hardware bring-up is done in dma_create() (all streams set up once). */
}

void dma_deinit(dma *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const). */
}

/* --- DMA stream ISR (registered once per stream via the irq framework) --- */
static void dma_isr(void *ctx)
{
    dma_stream_rt_t *st = (dma_stream_rt_t *)ctx;
    /* The stream has a dedicated IRQ line, so this ISR only needs to check its
     * own flags. Clear them (writing 1), then release the blocked waiter and run
     * the completion callback if one was supplied. */
    dma_hal_stream_t *hs = st->hal;
    if (dma_hal_stream_tc(hs) || dma_hal_stream_te(hs)) {
        dma_hal_stream_clear_flags(hs);
        osal_sem_give(&st->done_sem);
        if (st->cb) st->cb(st->cb_ctx);
    }
}

/* --- typed methods --- */

static dma_stream_t *dma_acquire(dma *self, uint8_t stream_idx, uint8_t channel, dma_dir_t dir)
{
    if (!self) return NULL;
    uint32_t i;
    if (stream_idx == DMA_STREAM_ANY) {
        /* memory-to-memory: any free stream will do. */
        for (i = 0; i < DMA_STREAMS_PER_CTLR; i++)
            if (!self->streams[i].in_use) break;
        if (i == DMA_STREAMS_PER_CTLR) {
            log_printf(app_log(), LOG_DEBUG, "dma", "%s: no free stream\n",
                       self->parent.parent.name);
            return NULL;
        }
    } else {
        /* peripheral request: the silicon demands a SPECIFIC stream. */
        if (stream_idx >= DMA_STREAMS_PER_CTLR) return NULL;
        i = stream_idx;
        if (self->streams[i].in_use) {
            log_printf(app_log(), LOG_DEBUG, "dma",
                       "%s: stream %u busy (requested for peripheral)\n",
                       self->parent.parent.name, (unsigned)i);
            return NULL;
        }
    }
    self->streams[i].in_use  = 1;
    self->streams[i].dir      = dir;
    self->streams[i].channel  = channel & 0x7U;
    self->streams[i].cb       = NULL;
    self->streams[i].cb_ctx   = NULL;
    return &self->handles[i];
}

static int dma_stream_config(dma *self, dma_stream_t *s, const void *periph,
                             void *mem, uint32_t count, dma_data_size_t size,
                             int periph_inc, int mem_inc, dma_prio_t prio)
{
    if (!self || !s) return -1;
    uint32_t i = (uint32_t)s->idx;
    dma_dir_t dir = self->streams[i].dir;
    /* STM32F4 hardware quirk: DMA1 cannot do memory-to-memory. Catch it up front
     * so the caller gets a clean -1 instead of a hung wait_done (EN sets but
     * NDTR never decrements, TC never fires). dma2 is the M2M-capable one. */
    if (dir == DMA_DIR_M2M && !dma_hal_is_m2m_capable(self->hal[i])) {
        log_printf(app_log(), LOG_DEBUG, "dma",
                   "%s: M2M not supported on this controller (use dma2)\n",
                   self->parent.parent.name);
        return -1;
    }
    dma_hal_dir_t hdir = (dma_hal_dir_t)dir;  /* captured at acquire */
    dma_hal_stream_config(self->hal[i], hdir, self->streams[i].channel,
                          periph, mem, count, (dma_hal_size_t)size,
                          periph_inc, mem_inc, (uint32_t)prio);
    return 0;
}

static int dma_stream_start(dma *self, dma_stream_t *s, void (*cb)(void *), void *ctx)
{
    if (!self || !s) return -1;
    uint32_t i = (uint32_t)s->idx;
    self->streams[i].cb     = cb;
    self->streams[i].cb_ctx = ctx;
    /* enable TC + TE interrupts while EN=0, THEN arm the stream (CR writable
     * only when EN=0, so this order is required). */
    dma_hal_stream_enable_irq(self->hal[i], 1, 1);
    dma_hal_stream_start(self->hal[i]);
    return 0;
}

static int dma_wait_done(dma *self, dma_stream_t *s, uint32_t timeout_ms)
{
    if (!self || !s) return -1;
    /* Poll the Transfer-Complete hardware flag with a bounded budget instead of
     * blocking on the completion semaphore forever. This (a) honours timeout_ms,
     * (b) avoids a permanent hang if a transfer is mis-configured (e.g. a wrong
     * peripheral route), and (c) keeps the driver usable from bare metal where
     * the sem is a busy-wait with no deadline. The TC/TE ISR still gives the sem
     * (and runs the optional callback); once we observe TC we consume any permit
     * left in the sem so the NEXT transfer's wait starts clean (no trywait in the
     * bare-metal OSAL). */
    dma_hal_stream_t *hs = self->hal[s->idx];
    uint32_t limit = (timeout_ms ? timeout_ms : 2000U) * 1000U;   /* ~1k iters/ms */
    while (!dma_hal_stream_tc(hs) && limit--) { }
    int ok = dma_hal_stream_tc(hs) ? 1 : 0;
    self->streams[s->idx].done_sem.count = 0;   /* drain any stale permit */
    return ok ? 0 : -1;
}

static int dma_poll_done(dma *self, dma_stream_t *s)
{
    if (!self || !s) return 0;
    return dma_hal_stream_tc(self->hal[s->idx]);
}

static uint32_t dma_remaining(dma *self, dma_stream_t *s)
{
    if (!self || !s) return 0;
    return dma_hal_stream_remaining(self->hal[s->idx]);
}

static void dma_stream_free(dma *self, dma_stream_t *s)
{
    if (!self || !s) return;
    uint32_t i = (uint32_t)s->idx;
    dma_hal_stream_stop(self->hal[i]);          /* disarm */
    dma_hal_stream_disable_irq(self->hal[i]);   /* silence TC/TE IE */
    self->streams[i].in_use = 0;
}

/* --- unified device-interface virtual implementations --- */

static int dma_dev_open(device *self)
{
    (void)self;
    return 0;   /* controller + streams are brought up once in dma_create() */
}

static int dma_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int dma_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }   /* DMA is not a byte stream */

static int dma_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

static int dma_dev_ioctl(device *self, int cmd, void *arg)
{
    dma *d = (dma *)self;
    switch (cmd) {
    case DMA_IOCTL_FREE_STREAMS: {
        if (!arg) return -1;
        uint32_t free = 0;
        for (uint32_t i = 0; i < DMA_STREAMS_PER_CTLR; i++)
            if (!d->streams[i].in_use) free++;
        *(uint32_t *)arg = free;
        return 0;
    }
    default:
        return -1;
    }
}

/* --- control-class ops --- */

static int dma_control_command(control_device *self, int cmd, void *arg)
{
    /* delegate to the device ioctl surface (single command today) */
    return dma_dev_ioctl((device *)self, cmd, arg);
}

static int dma_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int dma_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }
