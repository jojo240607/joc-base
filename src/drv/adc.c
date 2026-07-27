#include "adc.h"
#include "adc_hal.h"
#include "irq_manager.h"              /* centralized interrupt manager */
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */
#include "log/log.h"
#include "log/app_log.h"

/* virtual implementations dispatched through the unified device vtable */
static int adc_dev_open(device *self);
static int adc_dev_close(device *self);
static int adc_dev_read(device *self, void *buf, size_t len);
static int adc_dev_write(device *self, const void *buf, size_t len);
static int adc_dev_ioctl(device *self, int cmd, void *arg);
static void adc_hw_init(adc *self);
static void adc_isr(void *ctx);   /* EOC ISR (registered via the irq framework) */

/* subclass vtable (defined below; forward-declared so adc_init can reference it) */
static const struct stream_deviceVtable adc_stream_vtable;

/* public methods — `static`, reachable ONLY through self->fun-> (forward decls
 * so the const fun table below can reference them, per the moban template) */
static uint32_t adc_read(adc *self);
static uint32_t adc_read_mv(adc *self);
static void adc_set_channel(adc *self, uint32_t channel);

const struct adcFun adc_fun = {
    .destroy     = adc_destroy,
    .init        = adc_init,
    .deinit      = adc_deinit,
    .read        = adc_read,
    .read_mv     = adc_read_mv,
    .set_channel = adc_set_channel,
};

/* one shared vtable for the whole ADC class — assigned by adc_init() */
static const struct deviceVtable adc_dev_vtable = {
    .open  = adc_dev_open,
    .close = adc_dev_close,
    .read  = adc_dev_read,
    .write = adc_dev_write,
    .ioctl = adc_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *adc_create(const void *config)
{
    const adc_config_t *c = (const adc_config_t *)config;
    adc *self = (adc *)malloc(sizeof(adc));
    if (!self) return NULL;
    memset(self, 0, sizeof(adc));
    self->hal     = adc_hal_create(c->periph, c->channel);
    if (!self->hal) { free(self); return NULL; }   /* #9: HAL alloc failure */
    self->channel = c->channel;
    self->vdda_mv = c->vdda_mv;
    self->ain_signal = c->ain_signal;        /* cached for pinmux claim at open() */
    self->dma_req = c->dma_req;              /* cached for re-acquire on reopen */
    self->dma_dev = NULL; self->dma_str = NULL;
    /* Resolve the analog-input signal name up front (external channels only).
     * Internal channels (16/17/18) take no GPIO pin, so ain_signal is NULL. */
    if (c->channel < 16U && c->ain_signal) {
        pinmux_hal_resolve(c->ain_signal, &self->port, &self->pin, &self->af);
    }
    self->parent.parent.type = DEVICE_TYPE_ADC;     /* driver sets its own class */
    self->parent.parent.name = c->name;             /* driver sets its own name */
    adc_init(self);
    return (device *)self;
}

void adc_destroy(adc *self)
{
    if (!self) return;
    adc_deinit(self);
    adc_hal_destroy(self->hal);   /* mirror create: free the HAL handle */
    free(self);
}

void adc_init(adc *self)
{
    if (!self) return;
    self->parent.parent.vtable = &adc_dev_vtable;     /* base device vtable */
    self->parent.vtable        = &adc_stream_vtable;  /* stream-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_ADC;
    self->parent.parent.class  = DEVICE_CLASS_STREAM;
    self->parent.mode          = STREAM_MODE_IRQ;    /* EOC interrupt-driven conversion */
    self->fun = &adc_fun;
    /* hardware bring-up is deferred to open() (see adc_dev_open) */
}

void adc_deinit(adc *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
}

static uint32_t adc_read(adc *self)
{
    if (!self) return 0U;
    return adc_hal_single_convert(self->hal);
}

static uint32_t adc_read_mv(adc *self)
{
    if (!self) return 0U;
    return adc_hal_to_mv(adc_hal_single_convert(self->hal), self->vdda_mv);
}

static void adc_set_channel(adc *self, uint32_t channel)
{
    if (!self) return;
    self->channel = channel;
    adc_hal_set_channel(self->hal, channel);
}

/* --- DMA engine (STREAM_MODE_DMA, P2M: ADC_DR -> memory) ---
 * An ADC is hard-wired to ONE specific DMA stream (ADC1->DMA2_Stream0). We
 * resolve that stream once at open() (via dma_hal_route) and keep it reserved;
 * the DMA ISR (in drv/dma.c) gives the stream's done_sem on Transfer-Complete,
 * so the read blocks until the hardware finishes the whole burst — the blocking
 * API contract is preserved. */
static int adc_dma_acquire(adc *a)
{
    a->dma_dev = NULL; a->dma_str = NULL;
    if (a->dma_req == DMA_REQ_NONE) return 0;   /* this ADC has no DMA configured */
    dma_route_t rt = dma_hal_route(a->dma_req);
    if (!rt.name) return 0;
    dma *dm = (dma *)device_manager_get(rt.name);
    if (!dm) return 0;
    dma_stream_t *s = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_P2M);
    if (s) { a->dma_dev = dm; a->dma_str = s; }
    else log_printf(app_log(), LOG_DEBUG, "adc", "%s: DMA stream busy\n",
                   a->parent.parent.name);
    return (a->dma_str) ? 0 : -1;
}

/* Release the reserved DMA stream (called at close). */
static void adc_dma_release(adc *a)
{
    if (a->dma_dev && a->dma_str) a->dma_dev->fun->free(a->dma_dev, a->dma_str);
    a->dma_dev = NULL; a->dma_str = NULL;
}

/* --- Per-engine state management (lazy allocation, mirrors uart_setup_engine) ---
 * The engine (POLL/IRQ/DMA) is the only axis; only the active engine's state is
 * heap-allocated (adc_irq_t / adc_dma_t), reached via a single `a->eng` cast.
 * Returns 0 on success, -1 if the engine is unavailable (leaving a untouched). */
static void adc_free_engine(adc *a)
{
    if (!a->eng) return;
    free(a->eng);
    a->eng = NULL;
}

static int adc_setup_engine(adc *a, stream_xfer_mode_t engine)
{
    if (engine != STREAM_MODE_POLL && engine != STREAM_MODE_IRQ &&
        engine != STREAM_MODE_DMA)
        return -1;
    /* DMA needs the hard-wired stream reserved at open(). */
    if (engine == STREAM_MODE_DMA && !a->dma_str) return -1;
    /* IRQ needs a valid EOC interrupt line. */
    if (engine == STREAM_MODE_IRQ && a->eoc_irq < 0) return -1;

    /* tear down current state (after validation, so a rejection leaves the adc
     * in its previous, working configuration). */
    adc_free_engine(a);

    if (engine == STREAM_MODE_IRQ) {
        adc_irq_t *e = (adc_irq_t *)malloc(sizeof(adc_irq_t));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        a->eng = e;
    } else if (engine == STREAM_MODE_DMA) {
        adc_dma_t *e = (adc_dma_t *)malloc(sizeof(adc_dma_t));
        if (!e) return -1;
        memset(e, 0, sizeof(*e));
        a->eng = e;
    } else { /* POLL: zero state */
        a->eng = NULL;
    }

    a->parent.mode = engine;
    /* Select the RX producer: IRQ arms the EOC ISR (data path); DMA + POLL
     * silence it so the ISR can't steal a sample the DMA is moving. */
    if (engine == STREAM_MODE_IRQ) {
        irq_manager_set_priority(a->eoc_irq, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
        adc_hal_enable_eoc_irq(a->hal);
        irq_manager_enable(a->eoc_irq, adc_isr, a);
    } else {
        adc_hal_disable_eoc_irq(a->hal);
        if (a->eoc_irq >= 0)
            irq_manager_disable(a->eoc_irq, adc_isr, a);
    }
    return 0;
}

/* DMA burst read: program the reserved stream (P2M, PAR=DR, memory=bounce,
 * MINC), start a continuous conversion burst, block until Transfer-Complete,
 * then copy the 16-bit samples out to the caller's uint32_t buffer. The bounce
 * is in main SRAM because DMA cannot touch CCM (a caller stack array might be
 * in CCM). */
static int adc_dma_read(adc *a, void *buf, size_t len)
{
    adc_dma_t *e = (adc_dma_t *)a->eng;
    if (!e || !a->dma_dev || !a->dma_str) return -1;
    uint32_t nsamp = (uint32_t)(len / sizeof(uint32_t));
    if (nsamp == 0) return -1;
    uint16_t *tmp = (nsamp <= ADC_DMA_BOUNCE) ? e->dma_bounce
                                               : (uint16_t *)malloc(nsamp * sizeof(uint16_t));
    if (!tmp) return -1;
    void *dr = adc_hal_get_dr_addr(a->hal);
    a->dma_dev->fun->config(a->dma_dev, a->dma_str, dr, tmp, nsamp,
                            DMA_DATA_16, 0 /*periph_inc*/, 1 /*mem_inc*/, DMA_PRIO_MED);
    a->dma_dev->fun->start(a->dma_dev, a->dma_str, NULL, NULL);  /* arm stream (EN=1) BEFORE the ADC starts */
    adc_hal_enable_dma(a->hal);                 /* CR2 DMA|CONT + SWSTART (now generates DMA requests) */
    int rc = a->dma_dev->fun->wait_done(a->dma_dev, a->dma_str, 2000);
    adc_hal_disable_dma(a->hal);
    if (rc == 0) {
        uint32_t *out = (uint32_t *)buf;
        for (uint32_t i = 0; i < nsamp; i++) out[i] = tmp[i];
    }
    if (tmp != e->dma_bounce) free(tmp);
    return rc == 0 ? (int)(nsamp * sizeof(uint32_t)) : -1;
}

/* --- unified device-interface virtual implementations --- */

static int adc_dev_open(device *self)
{
    adc *a = (adc *)self;
    adc_hw_init(a);
    adc_dma_acquire(a);              /* reserve the hard-wired DMA stream (if any) */
    /* (Re)build the per-engine state for the chosen engine (default IRQ) and
     * select the EOC producer. If the configured engine is unavailable (e.g. IRQ
     * with no EOC line), fall back to POLL so the adc stays usable. */
    if (adc_setup_engine(a, a->parent.mode) != 0)
        adc_setup_engine(a, STREAM_MODE_POLL);
    return 0;
}

static int adc_dev_close(device *self)
{
    adc *a = (adc *)self;
    adc_free_engine(a);              /* free per-engine state (idempotent) */
    adc_dma_release(a);              /* release the reserved DMA stream */
    return 0;
}

/* stream-class ops — the REAL implementations; the base deviceVtable forwards
 * here. An ADC is a sampling stream: one read() returns one converted sample.
 * The transfer engine is chosen by self->parent.mode (POLL/IRQ/DMA). */
static int adc_stream_read(stream_device *self, void *buf, size_t len)
{
    adc *a = (adc *)self;
    if (len < sizeof(uint32_t)) return -1;
    if (a->parent.mode == STREAM_MODE_DMA) return adc_dma_read(a, buf, len);
    if (a->parent.mode == STREAM_MODE_IRQ) {
        adc_irq_t *e = (adc_irq_t *)a->eng;
        if (!e) return -1;
        /* interrupt-driven: trigger the conversion, block on the EOC semaphore,
         * then take the value the ISR stashed into last_raw. */
        osal_sem_init(&e->eoc_sem, 0);
        adc_hal_start_convert(a->hal);
        osal_sem_wait(&e->eoc_sem);
        *(uint32_t *)buf = e->last_raw;
        return (int)sizeof(uint32_t);
    }
    *(uint32_t *)buf = adc_read(a);    /* polling single conversion */
    return (int)sizeof(uint32_t);
}

/* EOC ISR (registered via the unified irq framework). Reads DR — this clears
 * EOC so the interrupt will not re-fire — and hands the result to the blocked
 * IRQ-mode reader via the completion semaphore. */
static void adc_isr(void *ctx)
{
    adc *a = (adc *)ctx;
    adc_irq_t *e = (adc_irq_t *)a->eng;
    if (!e) return;
    e->last_raw = adc_hal_read_dr(a->hal);
    osal_sem_give(&e->eoc_sem);
}
static int adc_stream_write(stream_device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }   /* ADC is read-only */
static int adc_stream_flush(stream_device *self)
    { (void)self; return 0; }
static int adc_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }
static int adc_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }

static const struct stream_deviceVtable adc_stream_vtable = {
    .read        = adc_stream_read,
    .write       = adc_stream_write,
    .flush       = adc_stream_flush,
    .read_frame  = adc_stream_read_frame,
    .write_frame = adc_stream_write_frame,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

/* base device-interface ops forward to the stream-class vtable */
static int adc_dev_read(device *self, void *buf, size_t len)
    { return adc_stream_read((stream_device *)self, buf, len); }
static int adc_dev_write(device *self, const void *buf, size_t len)
    { return adc_stream_write((stream_device *)self, buf, len); }

static int adc_dev_ioctl(device *self, int cmd, void *arg)
{
    adc *a = (adc *)self;
    switch (cmd) {
    case ADC_IOCTL_SET_CHANNEL:
        if (!arg) return -1;
        adc_set_channel(a, *(const uint32_t *)arg);
        return 0;
    case ADC_IOCTL_GET_CHANNEL:
        if (!arg) return -1;
        *(uint32_t *)arg = a->channel;
        return 0;
    case ADC_IOCTL_SET_VREF_MV:
        if (!arg) return -1;
        a->vdda_mv = *(const uint32_t *)arg;
        return 0;
    case ADC_IOCTL_READ_MV:
        if (!arg) return -1;
        *(uint32_t *)arg = adc_read_mv(a);
        return 0;
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        /* (re)build per-engine state for the new engine (frees the old, allocates
         * the new, selects the EOC producer). Returns -1 (leaving the adc in its
         * previous configuration) if the engine is unavailable. */
        return adc_setup_engine(a, m);
    }
    case STREAM_IOCTL_GET_MODE:
        if (!arg) return -1;
        *(stream_xfer_mode_t *)arg = a->parent.mode;
        return 0;
    default:
        return -1;
    }
}

/* --- hardware bring-up (delegated to HAL via the opaque handle) --- */
static void adc_hw_init(adc *self)
{
    adc_hal_enable_clock(self->hal);
    adc_hal_common_config(self->hal);

    /* Claim + program the analog input pin through the pinmux BEFORE touching
     * the GPIO registers. A conflict (pin already owned elsewhere) makes the
     * request fail and we refuse to configure it. Internal channels
     * (16/17/18) need no GPIO pin, so skip them. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm && self->channel < 16U) {
        if (pm->fun->request(pm, self->port, self->pin, self->af, self->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "adc", "%s: pin P%c%d CONFLICT — refused",
                   self->parent.parent.name, 'A' + self->port, self->pin);
            return;                          /* conflict: do NOT configure */
        }
        pinmux_pin_cfg_t cfg = {
            .af = self->af, .mode = 3, .otype = 0, .speed = 0, .pupd = 0
        };
        pm->fun->config(pm, self->port, self->pin, &cfg);
    } else {
        adc_hal_config_gpio(self->hal);      /* fallback when no pinmux / internal ch */
    }

    adc_hal_config_channel(self->hal);

    /* Register the EOC ISR through the platform-independent irq framework. The
     * callback is installed once at open(); the EOC interrupt itself is armed /
     * disarmed by adc_setup_engine() according to the active engine (IRQ arms it,
     * DMA/POLL silence it). */
    self->eoc_irq = adc_hal_irq_id(self->hal);
    irq_manager_attach(self->eoc_irq, adc_isr, self);  /* register handler */
}
