#include "timer.h"
#include "irq.h"             /* platform-independent interrupt framework */
#include "irq_manager.h"     /* centralized interrupt manager */
#include <stdlib.h>
#include <string.h>

/* virtual impls dispatched through the event_device vtable */
static int timer_set_cb(event_device *self, device_event_type_t ev,
                        device_event_cb_t cb, void *ctx);
static int timer_clear_cb(event_device *self, device_event_type_t ev);
static int timer_enable(event_device *self);
static int timer_disable(event_device *self);

/* minimal base device vtable — event devices do not bulk read/write */
static int timer_dev_open(device *self);
static int timer_dev_close(device *self);
static int timer_dev_read(device *self, void *buf, size_t len);
static int timer_dev_write(device *self, const void *buf, size_t len);
static int timer_dev_ioctl(device *self, int cmd, void *arg);

static const struct event_deviceVtable timer_evt_vtable = {
    .set_event_callback   = timer_set_cb,
    .clear_event_callback = timer_clear_cb,
    .enable  = timer_enable,
    .disable = timer_disable,
};

static const struct deviceVtable timer_dev_vtable = {
    .open  = timer_dev_open,
    .close = timer_dev_close,
    .read  = timer_dev_read,
    .write = timer_dev_write,
    .ioctl = timer_dev_ioctl,
};

/* The overflow ISR: runs in interrupt context via the unified irq framework.
 * Because two timers can share one IRQ line (TIM1_UP + TIM10 on IRQ 25,
 * TIM8_UP + TIM13 on IRQ 44), this handler FIRST checks whether ITS OWN timer
 * raised UIF. If a sibling on the shared line was the real trigger, we return
 * immediately without touching our counter. Only then do we clear UIF (or the
 * interrupt re-enters immediately) and notify the subscriber. */
static void timer_isr(void *ctx)
{
    timer *t = (timer *)ctx;
    if (!tim_hal_uif_pending(t->hal))
        return;                         /* shared line: sibling peripheral fired */
    tim_hal_clear_uif(t->hal);          /* re-arm: must clear before returning */
    t->overflows++;
    if (t->cb)
        t->cb(t->cb_ctx, DEVICE_EVENT_TICK, (void *)&t->overflows);
}

static int timer_set_cb(event_device *self, device_event_type_t ev,
                        device_event_cb_t cb, void *ctx)
{
    if (ev != DEVICE_EVENT_TICK)
        return -1;
    timer *t = (timer *)self;
    t->cb = cb;
    t->cb_ctx = ctx;
    return 0;
}

static int timer_clear_cb(event_device *self, device_event_type_t ev)
{
    if (ev != DEVICE_EVENT_TICK)
        return -1;
    timer *t = (timer *)self;
    t->cb = NULL;
    t->cb_ctx = NULL;
    return 0;
}

/* enable = start counting AND arm the NVIC (the ISR is already attached at
 * open, so irq_manager_enable is safe: callback present). */
static int timer_enable(event_device *self)
{
    timer *t = (timer *)self;
    tim_hal_start(t->hal);
    irq_manager_enable(t->irq, timer_isr, t);
    return 0;
}

static int timer_disable(event_device *self)
{
    timer *t = (timer *)self;
    irq_manager_disable(t->irq, timer_isr, t);
    tim_hal_stop(t->hal);
    return 0;
}

static int timer_dev_open(device *self)
{
    timer *t = (timer *)self;
    tim_hal_enable_clock(t->hal);
    tim_hal_config(t->hal, t->timer_clk_hz, t->tick_hz);
    tim_hal_set_repetition(t->hal, t->repetition);   /* RCR (adv TIM; no-op on GP) */
    tim_hal_enable_update_irq(t->hal);   /* peripheral UIE (gated by class) */
    irq_manager_set_priority(t->irq, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    irq_manager_attach(t->irq, timer_isr, t);   /* register handler */
    return 0;
}

static int timer_dev_close(device *self)
{
    timer_disable((event_device *)self);
    timer *t = (timer *)self;
    irq_manager_detach(t->irq, timer_isr, t);   /* mask NVIC + uninstall callback */
    return 0;
}

static int timer_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int timer_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

static int timer_dev_ioctl(device *self, int cmd, void *arg)
{
    timer *t = (timer *)self;
    switch (cmd) {
    case TIMER_IOCTL_GET_OVERFLOWS:
        if (arg) *(uint32_t *)arg = t->overflows;
        return 0;
    case TIMER_IOCTL_GET_COUNTER:
        if (arg) *(uint32_t *)arg = tim_hal_get_counter(t->hal);
        return 0;
    case TIMER_IOCTL_SET_REPETITION:
        if (!arg) return -1;
        t->repetition = *(const uint32_t *)arg;
        return tim_hal_set_repetition(t->hal, t->repetition);  /* -1 on GP TIM */
    case TIMER_IOCTL_GET_REPETITION:
        if (arg) *(uint32_t *)arg = tim_hal_get_repetition(t->hal);
        return 0;
    default:
        return -1;
    }
}

device *timer_create(const void *config)
{
    const timer_config_t *c = (const timer_config_t *)config;
    if (!c || c->tick_hz == 0 || c->timer_clk_hz < c->tick_hz)
        return NULL;

    timer *t = (timer *)malloc(sizeof(timer));
    if (!t) return NULL;
    memset(t, 0, sizeof(timer));

    t->hal = tim_hal_create(c->peripheral);
    if (!t->hal) { free(t); return NULL; }

    t->parent.parent.vtable = &timer_dev_vtable;
    t->parent.parent.type   = DEVICE_TYPE_TIMER;
    t->parent.parent.class  = DEVICE_CLASS_EVENT;   /* four-class tag */
    t->parent.parent.name   = c->name;
    t->parent.vtable        = &timer_evt_vtable;
    t->irq = tim_hal_irq_id(t->hal);
    t->timer_clk_hz = c->timer_clk_hz;
    t->tick_hz = c->tick_hz;
    t->repetition = 0;
    t->overflows = 0;
    t->cb = NULL;
    t->cb_ctx = NULL;

    return &t->parent.parent;
}

void timer_destroy(timer *self)
{
    if (!self) return;
    irq_manager_detach(self->irq, timer_isr, self);
    tim_hal_destroy(self->hal);
    free(self);
}
