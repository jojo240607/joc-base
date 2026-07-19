#ifndef CONTROL_DEVICE_H
#define CONTROL_DEVICE_H

#include "iface/device.h"
#include <stddef.h>

/*
 * 控制型设备 (Control) base class — a subclass of `device`.
 *
 * Covers GPIO output, PWM, clock source, PinMux, watchdog, power management,
 * simple DAC. Dominated by parameter/state commands; little or no bulk
 * transfer. The base `device` gives open/close/ioctl; this class adds the
 * command/set/get triplet (an ioctl-style control surface) its members use.
 */

typedef struct _control_device control_device;

struct control_deviceVtable {
    int (*command)(control_device *self, int cmd, void *arg);  /* ioctl-style */
    int (*set)(control_device *self, int param, const void *val);
    int (*get)(control_device *self, int param, void *val);
};

struct _control_device {
    device parent;
    const struct control_deviceVtable *vtable;
};

static inline device *control_device_to_device(control_device *c) { return &c->parent; }
static inline control_device *device_as_control(device *d)
    { return (d && d->class == DEVICE_CLASS_CONTROL) ? (control_device *)d : NULL; }

#endif /* CONTROL_DEVICE_H */
