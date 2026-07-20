#include "systick.h"
#include "irq.h"             /* platform-independent interrupt framework */
#include "irq_manager.h"     /* centralized interrupt manager */
#include "irq_hal.h"         /* chip HAL: SysTick id + config (driver stays clean) */
#include <stdlib.h>
#include <string.h>

/* virtual impls dispatched through the event_device vtable */
static int systick_set_cb(event_device *self, device_event_type_t ev,
                          device_event_cb_t cb, void *ctx);
static int systick_clear_cb(event_device *self, device_event_type_t ev);
static int systick_enable(event_device *self);
static int systick_disable(event_device *self);

/* minimal base device vtable — event devices do not bulk read/write */
static int systick_dev_open(device *self);
static int systick_dev_close(device *self);
static int systick_dev_read(device *self, void *buf, size_t len);
static int systick_dev_write(device *self, const void *buf, size_t len);
static int systick_dev_ioctl(device *self, int cmd, void *arg);

static const struct event_deviceVtable systick_evt_vtable = {
    .set_event_callback   = systick_set_cb,
    .clear_event_callback = systick_clear_cb,
    .enable  = systick_enable,
    .disable = systick_disable,
};

static const struct deviceVtable systick_dev_vtable = {
    .open  = systick_dev_open,
    .close = systick_dev_close,
    .read  = systick_dev_read,
    .write = systick_dev_write,
    .ioctl = systick_dev_ioctl,
};

/* The tick ISR: runs in interrupt context via the unified irq framework.
 * Increment the tick counter and notify the subscriber. */
static void systick_isr(void *ctx)
{
    systick *s = (systick *)ctx;
    s->ticks++;
    irq_hal_systick_clear();    /* read clears COUNTFLAG, re-arms the tick */
    if (s->cb)
        s->cb(s->cb_ctx, DEVICE_EVENT_TICK, (void *)&s->ticks);
}

static int systick_set_cb(event_device *self, device_event_type_t ev,
                          device_event_cb_t cb, void *ctx)
{
    if (ev != DEVICE_EVENT_TICK)
        return -1;
    systick *s = (systick *)self;
    s->cb = cb;
    s->cb_ctx = ctx;
    return 0;
}

static int systick_clear_cb(event_device *self, device_event_type_t ev)
{
    if (ev != DEVICE_EVENT_TICK)
        return -1;
    systick *s = (systick *)self;
    s->cb = NULL;
    s->cb_ctx = NULL;
    return 0;
}

static int systick_enable(event_device *self)
{
    systick *s = (systick *)self;
    irq_manager_enable(irq_hal_systick_id(), systick_isr, s);   /* arm (cb attached) */
    return 0;
}

static int systick_disable(event_device *self)
{
    systick *s = (systick *)self;
    irq_manager_disable(irq_hal_systick_id(), systick_isr, s);
    return 0;
}

static int systick_dev_open(device *self)  { (void)self; return 0; }
static int systick_dev_close(device *self)
{
    systick_disable((event_device *)self);
    return 0;
}
static int systick_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int systick_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int systick_dev_ioctl(device *self, int cmd, void *arg)
    { (void)self; (void)cmd; (void)arg; return -1; }

device *systick_create(const void *config)
{
    const systick_config_t *c = (const systick_config_t *)config;
    if (!c || c->tick_hz == 0 || c->cpu_hz < c->tick_hz)
        return NULL;

    systick *s = (systick *)malloc(sizeof(systick));
    if (!s) return NULL;
    memset(s, 0, sizeof(systick));

    s->parent.parent.vtable = &systick_dev_vtable;
    s->parent.parent.type   = DEVICE_TYPE_SYSTICK;
    s->parent.parent.class  = DEVICE_CLASS_EVENT;   /* <-- the four-class tag */
    s->parent.parent.name   = c->name;
    s->parent.vtable        = &systick_evt_vtable;
    s->ticks = 0;
    s->cb = NULL;
    s->cb_ctx = NULL;

    /* configure the Cortex-M core timer via the chip HAL (no raw CMSIS in the
     * driver); the tick rate is generic, the silicon detail stays in irq_hal. */
    if (irq_hal_systick_config(c->cpu_hz, c->tick_hz) != 0)
        return NULL;

    /* register the ISR through the PLATFORM-INDEPENDENT irq framework — no
     * vector-table / NVIC code lives in this driver. */
    irq_id_t id = irq_hal_systick_id();
    irq_set_priority(id, 0);
    irq_manager_attach(id, systick_isr, s);   /* register handler */
    irq_manager_enable(id, systick_isr, s);   /* arm NVIC */

    return &s->parent.parent;
}

void systick_destroy(systick *self)
{
    if (!self) return;
    irq_id_t id = irq_hal_systick_id();
    irq_manager_detach(id, systick_isr, self);   /* mask NVIC + uninstall callback */
    free(self);
}
