#ifndef EVENT_DEVICE_H
#define EVENT_DEVICE_H

#include "iface/device.h"
#include <stddef.h>

/*
 * 事件型设备 (Event) base class — a subclass of `device`.
 *
 * Covers keys, rotary encoders, external interrupts, timer alarms, RTC
 * periodic wakeup, semaphore triggers. These are ASYNCHRONOUS notification
 * sources: they have NO bulk read/write. The driver registers its ISR via the
 * generic irq framework (irq_register); subscribers are notified through the
 * callback. So ALL event devices share ONE interrupt engine.
 */

typedef enum {
    DEVICE_EVENT_IRQ,     /* a hardware interrupt line fired */
    DEVICE_EVENT_TICK,    /* periodic timer tick */
    DEVICE_EVENT_DATA,    /* data ready to be read */
    DEVICE_EVENT_ERROR,   /* error / exception */
} device_event_type_t;

typedef void (*device_event_cb_t)(void *ctx, device_event_type_t ev, void *ev_data);

typedef struct _event_device event_device;

struct event_deviceVtable {
    int (*set_event_callback)(event_device *self, device_event_type_t ev,
                              device_event_cb_t cb, void *ctx);
    int (*clear_event_callback)(event_device *self, device_event_type_t ev);
    int (*enable)(event_device *self);    /* arm the irq source (irq_enable) */
    int (*disable)(event_device *self);   /* disarm (irq_disable) */
};

struct _event_device {
    device parent;
    const struct event_deviceVtable *vtable;
};

static inline device *event_device_to_device(event_device *e) { return &e->parent; }
static inline event_device *device_as_event(device *d)
    { return (d && d->class == DEVICE_CLASS_EVENT) ? (event_device *)d : NULL; }

#endif /* EVENT_DEVICE_H */
