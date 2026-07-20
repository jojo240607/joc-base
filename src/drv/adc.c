#include "adc.h"
#include "adc_hal.h"
#include "irq_manager.h"              /* centralized interrupt manager */
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */

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

/* --- unified device-interface virtual implementations --- */

static int adc_dev_open(device *self)
{
    adc_hw_init((adc *)self);
    return 0;
}

static int adc_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* stream-class ops — the REAL implementations; the base deviceVtable forwards
 * here. An ADC is a sampling stream: one read() returns one converted sample.
 * The transfer engine is chosen by self->parent.mode (POLL/IRQ/DMA). */
static int adc_stream_read(stream_device *self, void *buf, size_t len)
{
    adc *a = (adc *)self;
    if (len < sizeof(uint32_t)) return -1;
    if (a->parent.mode == STREAM_MODE_DMA) return -1;   /* no DMA engine here */
    if (a->parent.mode == STREAM_MODE_IRQ) {
        /* interrupt-driven: trigger the conversion, block on the EOC semaphore,
         * then take the value the ISR stashed into last_raw. */
        osal_sem_init(&a->eoc_sem, 0);
        adc_hal_start_convert(a->hal);
        osal_sem_wait(&a->eoc_sem);
        *(uint32_t *)buf = a->last_raw;
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
    a->last_raw = adc_hal_read_dr(a->hal);
    osal_sem_give(&a->eoc_sem);
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
        if (m == STREAM_MODE_DMA) return -1;   /* no DMA engine here */
        a->parent.mode = m;
        if (m == STREAM_MODE_IRQ) {
            irq_set_priority(a->eoc_irq, 1);
            adc_hal_enable_eoc_irq(a->hal);
            irq_manager_enable(a->eoc_irq, adc_isr, a);    /* arm NVIC (cb attached) */
        } else {
            adc_hal_disable_eoc_irq(a->hal);
            irq_manager_disable(a->eoc_irq, adc_isr, a);   /* mask NVIC (cb stays) */
        }
        return 0;
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
            printf("[adc] %s: pin P%c%d CONFLICT — refused\r\n",
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
     * callback is installed once at open(); the EOC interrupt itself is only
     * enabled when the stream is in STREAM_MODE_IRQ (here, if it already is, or
     * later via STREAM_IOCTL_SET_MODE). */
    self->eoc_irq = adc_hal_irq_id(self->hal);
    irq_set_priority(self->eoc_irq, 1);
    if (self->parent.mode == STREAM_MODE_IRQ)
        adc_hal_enable_eoc_irq(self->hal);      /* peripheral EOC IE (gated by mode) */
    irq_manager_attach(self->eoc_irq, adc_isr, self);  /* register handler */
    if (self->parent.mode == STREAM_MODE_IRQ)
        irq_manager_enable(self->eoc_irq, adc_isr, self); /* arm NVIC in IRQ mode */
}
