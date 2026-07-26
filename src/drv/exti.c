#include "exti.h"
#include "irq.h"             /* platform-independent interrupt framework */
#include "irq_manager.h"     /* centralized interrupt manager */
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include "pinmux_hal.h"               /* pinmux_port_t (resolved port for claim) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/* virtual impls dispatched through the event_device vtable */
static int exti_set_cb(event_device *self, device_event_type_t ev,
                       device_event_cb_t cb, void *ctx);
static int exti_clear_cb(event_device *self, device_event_type_t ev);
static int exti_enable(event_device *self);
static int exti_disable(event_device *self);

/* minimal base device vtable */
static int exti_dev_open(device *self);
static int exti_dev_close(device *self);
static int exti_dev_read(device *self, void *buf, size_t len);
static int exti_dev_write(device *self, const void *buf, size_t len);
static int exti_dev_ioctl(device *self, int cmd, void *arg);

static const struct event_deviceVtable exti_evt_vtable = {
    .set_event_callback   = exti_set_cb,
    .clear_event_callback = exti_clear_cb,
    .enable  = exti_enable,
    .disable = exti_disable,
};

static const struct deviceVtable exti_dev_vtable = {
    .open  = exti_dev_open,
    .close = exti_dev_close,
    .read  = exti_dev_read,
    .write = exti_dev_write,
    .ioctl = exti_dev_ioctl,
};

/* The EXTI ISR: runs in interrupt context via the unified irq framework.
 * Pins 5..9 share IRQ23 and pins 10..15 share IRQ40, so this handler MAY be
 * invoked because a SIBLING pin on the same NVIC line fired. We FIRST check
 * whether THIS pin's PR bit is set; if not, return immediately (sibling guard).
 * Only then do we clear the pending bit (or the interrupt re-enters) and notify
 * the subscriber. */
static void exti_isr(void *ctx)
{
    exti *e = (exti *)ctx;
    if (!exti_hal_pending(e->hal))
        return;                         /* shared line: sibling pin fired */
    exti_hal_clear(e->hal);            /* re-arm: must clear before returning */
    e->count++;
    if (e->cb)
        e->cb(e->cb_ctx, DEVICE_EVENT_IRQ, (void *)&e->pin);
}

static int exti_set_cb(event_device *self, device_event_type_t ev,
                       device_event_cb_t cb, void *ctx)
{
    if (ev != DEVICE_EVENT_IRQ)
        return -1;
    exti *e = (exti *)self;
    e->cb = cb;
    e->cb_ctx = ctx;
    return 0;
}

static int exti_clear_cb(event_device *self, device_event_type_t ev)
{
    if (ev != DEVICE_EVENT_IRQ)
        return -1;
    exti *e = (exti *)self;
    e->cb = NULL;
    e->cb_ctx = NULL;
    return 0;
}

static int exti_enable(event_device *self)
{
    exti *e = (exti *)self;
    irq_manager_enable(e->irq, exti_isr, e);   /* arm NVIC (cb already attached) */
    return 0;
}

static int exti_disable(event_device *self)
{
    exti *e = (exti *)self;
    irq_manager_disable(e->irq, exti_isr, e);  /* mask NVIC (cb stays) */
    exti_hal_mask(e->hal);                      /* also mask the EXTI line */
    return 0;
}

static int exti_dev_open(device *self)
{
    exti *e = (exti *)self;

    /* Claim the pin as an INPUT through the pinmux (af=0). A conflict makes the
     * request fail and we refuse to touch the hardware. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, e->port, e->pin, e->af, e->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "exti", "[exti] %s: pin P%c%d CONFLICT — refused\n",
                   e->parent.parent.name, 'A' + (int)e->port, (int)e->pin);
            return -2;
        }
        pinmux_pin_cfg_t cfg = {
            .af = 0, .mode = 0, .otype = 0, .speed = 0, .pupd = e->pupd
        };
        pm->fun->config(pm, e->port, e->pin, &cfg);
    }

    exti_hal_select_source(e->hal);   /* route port -> EXTI line (SYSCFG) */
    exti_hal_set_edge(e->hal, e->edge);
    exti_hal_unmask(e->hal);          /* line live (NVIC still armed by enable) */
    irq_manager_set_priority(e->irq, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    irq_manager_attach(e->irq, exti_isr, e);   /* register handler */
    e->count = 0;                     /* fresh counter each open (the device struct
                                       * is created once at board-init and survives
                                       * close(), so without this the boot BIST's
                                       * counts would leak into later self-tests) */
    return 0;
}

static int exti_dev_close(device *self)
{
    exti *e = (exti *)self;
    exti_disable((event_device *)self);
    irq_manager_detach(e->irq, exti_isr, e);   /* mask NVIC + uninstall callback */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, e->parent.parent.name);
    return 0;
}

static int exti_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int exti_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

static int exti_dev_ioctl(device *self, int cmd, void *arg)
{
    exti *e = (exti *)self;
    switch (cmd) {
    case EXTI_IOCTL_GET_COUNT:
        if (arg) *(uint32_t *)arg = e->count;
        return 0;
    case EXTI_IOCTL_TRIGGER:
        exti_hal_software_trigger(e->hal);   /* simulate an edge (self-test) */
        return 0;
    default:
        return -1;
    }
}

device *exti_create(const void *config)
{
    const exti_config_t *c = (const exti_config_t *)config;
    if (!c || !c->pin_signal) return NULL;

    pinmux_port_t port; uint8_t pin, af;
    if (!pinmux_hal_resolve(c->pin_signal, &port, &pin, &af)) {
        log_printf(app_log(), LOG_DEBUG, "exti", "[exti] %s: unknown signal \"%s\"\n", c->name, c->pin_signal);
        return NULL;
    }

    exti *e = (exti *)malloc(sizeof(exti));
    if (!e) return NULL;
    memset(e, 0, sizeof(exti));

    e->hal = exti_hal_create(port, pin);
    if (!e->hal) { free(e); return NULL; }

    e->parent.parent.vtable = &exti_dev_vtable;
    e->parent.vtable        = &exti_evt_vtable;
    e->parent.parent.type   = DEVICE_TYPE_EXTI;
    e->parent.parent.class  = DEVICE_CLASS_EVENT;   /* exti IS-A event_device */
    e->parent.parent.name   = c->name;
    e->irq   = exti_hal_irq_id(e->hal);
    e->edge  = c->edge;
    e->pin_signal = c->pin_signal;
    e->port  = port;
    e->pin   = pin;
    e->af    = af;
    e->pupd  = c->pupd;
    e->count = 0;
    e->cb    = NULL;
    e->cb_ctx = NULL;

    return &e->parent.parent;
}

void exti_destroy(exti *self)
{
    if (!self) return;
    irq_manager_detach(self->irq, exti_isr, self);
    exti_hal_destroy(self->hal);
    free(self);
}
