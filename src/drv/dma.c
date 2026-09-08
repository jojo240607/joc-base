#include "dma.h"
#include "dma_hal.h"
#include "irq_manager.h"              /* centralized interrupt manager */
#include <stdlib.h>
#include <string.h>
#include "log/log.h"
#include "log/app_log.h"
#include "rtos.h"

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
static int dma_stream_stop(dma *self, dma_stream_t *s);
static int dma_stream_start_circular(dma *self, dma_stream_t *s);
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
    .stop        = dma_stream_stop,
    .start_circular = dma_stream_start_circular,
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
    self->streams[i].channel  = channel;     /* F4 HAL masks to 3-bit CHSEL; H7 stores full DMAMUX ID */
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

/* Pause the stream (EN=0, TC/TE IE off) WITHOUT releasing it to the pool.
 * Differs from dma_stream_free, which also clears in_use. Here the owner keeps
 * the reservation so it can re-arm the same stream later (e.g. the UART idle-
 * line RX circular stream being halted before a bulk re-program). */
static int dma_stream_stop(dma *self, dma_stream_t *s)
{
    if (!self || !s) return -1;
    uint32_t i = (uint32_t)s->idx;
    dma_hal_stream_stop(self->hal[i]);          /* EN=0 */
    dma_hal_stream_disable_irq(self->hal[i]);   /* silence TC/TE IE */
    return 0;
}

/* Circular arm for the UART idle-line receiver: set CIRC=1 and EN=1 with NO
 * TC/TE interrupt (the DMA must run forever without raising TC; the UART IDLE
 * ISR reads NDTR to learn how many bytes arrived). config() already left EN=0,
 * so set_circular() (which also waits for EN=0) then arm() is safe. */
static int dma_stream_start_circular(dma *self, dma_stream_t *s)
{
    if (!self || !s) return -1;
    uint32_t i = (uint32_t)s->idx;
    self->streams[i].cb     = NULL;
    self->streams[i].cb_ctx = NULL;
    dma_hal_stream_disable_irq(self->hal[i]);   /* silent: no TC/TE ISR */
    dma_hal_stream_set_circular(self->hal[i], 1);
    dma_hal_stream_start(self->hal[i]);
    return 0;
}

static int dma_wait_done(dma *self, dma_stream_t *s, uint32_t timeout_ms)
{
    if (!self || !s) return -1;
    /* Wait on the completion SEMAPHORE the TC/TE ISR gives, NOT by polling the TC
     * flag. The ISR clears TC the instant it is set, so a polling loop would
     * typically read TC=0 (already cleared by the ISR) and time out even though
     * the transfer SUCCEEDED — a TOCTOU race. The sem permit, once given, PERSISTS
     * until this wait consumes it, so completion is never missed. Bounded by a
     * coarse instruction budget so a mis-routed transfer cannot hang the caller
     * forever. (Bare-metal OSAL busy-waits; RTOS OSAL blocks — both fine here.) */
    osal_sem_t *sem = &self->streams[s->idx].done_sem;
    dma_hal_stream_t *hs = self->hal[s->idx];
    uint32_t limit = (timeout_ms ? timeout_ms : 2000U) * 1000U;   /* ~1k iters/ms */
    while (limit--) {
        if (sem->count > 0) {                 /* completion permit observed */
            while (sem->count > 0) osal_sem_wait(sem);   /* consume (drain extras) */
            return 0;
        }
        if (dma_hal_stream_te(hs)) {          /* transfer error (TE) */
            while (sem->count > 0) osal_sem_wait(sem);
            return -1;
        }
    }
    while (sem->count > 0) osal_sem_wait(sem);   /* drain on timeout too */
    return -1;
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

/* ---------------------------------------------------------------------------
 * DMA self-test (M2M, Renode-only). selftest.c is excluded from H750 builds,
 * so this lives here gated by RTOS_SELFTEST + JOC_RENODE + STM32H750xx.
 * ------------------------------------------------------------------------- */
#if RTOS_SELFTEST && defined(JOC_RENODE) && defined(STM32H750xx)
#include "device_manager.h"
int dma_m2m_selftest(void)
{
    device *d = device_manager_get("dma2");
    if (!d) { log_printf(app_log(), LOG_DEBUG, "dma", "selftest: dma2 not found\n"); return 0; }
    d->vtable->open(d);

    dma *dm = (dma *)d;
    int pass = 1;

    /* 8-bit, 64-byte M2M copy */
    static uint8_t src8[64], dst8[64];
    for (int i = 0; i < 64; i++) { src8[i] = (uint8_t)(i * 3 + 1); dst8[i] = 0; }
    dma_stream_t *s8 = dm->fun->acquire(dm, DMA_STREAM_ANY, 0, DMA_DIR_M2M);
    int ok8 = 0;
    if (s8) {
        int rc = 0;
        rc |= dm->fun->config(dm, s8, src8, dst8, 64, DMA_DATA_8, 1, 1, DMA_PRIO_MED);
        rc |= dm->fun->start(dm, s8, NULL, NULL);
        rc |= dm->fun->wait_done(dm, s8, 0);
        ok8 = (rc == 0);
        for (int i = 0; i < 64; i++) if (dst8[i] != src8[i]) ok8 = 0;
        dm->fun->free(dm, s8);
    }
    if (!ok8) pass = 0;
    log_printf(app_log(), LOG_DEBUG, "dma", "M2M selftest: 8-bit %s\n", ok8 ? "PASS" : "FAIL");

    /* 32-bit, 128-byte (32 items) M2M copy */
    static uint32_t src32[32], dst32[32];
    for (int i = 0; i < 32; i++) { src32[i] = 0xDEAD0000u + (uint32_t)i; dst32[i] = 0; }
    dma_stream_t *s32 = dm->fun->acquire(dm, DMA_STREAM_ANY, 0, DMA_DIR_M2M);
    int ok32 = 0;
    if (s32) {
        int rc = 0;
        rc |= dm->fun->config(dm, s32, src32, dst32, 32, DMA_DATA_32, 1, 1, DMA_PRIO_HIGH);
        rc |= dm->fun->start(dm, s32, NULL, NULL);
        rc |= dm->fun->wait_done(dm, s32, 0);
        ok32 = (rc == 0);
        for (int i = 0; i < 32; i++) if (dst32[i] != src32[i]) ok32 = 0;
        dm->fun->free(dm, s32);
    }
    if (!ok32) pass = 0;
    log_printf(app_log(), LOG_DEBUG, "dma", "M2M selftest: 32-bit %s\n", ok32 ? "PASS" : "FAIL");

    d->vtable->close(d);
    return pass;
}
RTOS_SELFTEST_ADD("dma_m2m", dma_m2m_selftest);
#endif /* RTOS_SELFTEST && JOC_RENODE && STM32H750xx */
