#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>
#include <stddef.h>

/*
 * Unified driver interface (the "device" base / interface class).
 *
 * This is the SINGLE, platform-independent contract that EVERY peripheral
 * driver implements. It follows the moban/ template's `Ibase` pattern:
 *
 *   - The interface carries ONLY a virtual-function table (vtable) and NO
 *     `fun` table and NO state, so it holds zero platform knowledge and can
 *     be ported untouched.
 *   - The vtable is a POINTER: `struct deviceVtable *vtable;` (see Ibase.h).
 *   - Default virtual implementations live in device.c as `static` functions
 *     and are wired up by device_init(); they are NEVER defined in this header.
 *
 * A concrete driver (e.g. drv/adc_stm32) "inherits" this interface by
 * embedding `device parent;` as the FIRST member and overriding the vtable
 * entries in its own init():
 *
 *     self->parent.vtable->open  = adc_dev_open;   // override the virtual
 *     self->parent.vtable->read  = adc_dev_read;
 *     ...
 *
 * Because `device parent` is the first member, a `device *` and a driver
 * pointer alias the same address, so the upper layer holds a `device *` and
 * drives ANY peripheral through the SAME virtual dispatch:
 *
 *     dev->vtable->open(dev);                  // bring up / (re)configure
 *     n = dev->vtable->read(dev, buf, len);    // read data
 *     n = dev->vtable->write(dev, buf, len);   // write data
 *     r = dev->vtable->ioctl(dev, cmd, arg);   // device-specific control
 *     dev->vtable->close(dev);                 // tear down
 *
 * This is the exact C equivalent of Java's `dev.open()` on a polymorphic
 * reference: the call is dispatched through the object's vtable.
 */
typedef struct _device device;

/* Common device-control commands shared by all drivers (driver-specific ones
 * are defined in each drv header). */
#define DEVICE_IOCTL_INVALID  0x00

struct deviceVtable {
    int (*open)(device *self);                          /* bring up / (re)configure */
    int (*close)(device *self);                         /* tear down */
    int (*read)(device *self, void *buf, size_t len);   /* bytes read, <0 on error */
    int (*write)(device *self, const void *buf, size_t len); /* bytes written, <0 */
    int (*ioctl)(device *self, int cmd, void *arg);     /* device-specific control */
};

struct _device {
    struct deviceVtable *vtable;     /* virtual-function table (pointer, per Ibase) */
};

/* interface lifecycle: allocate / free the vtable and install the default
 * (no-op / unsupported) virtual implementations. A driver calls this from its
 * own init() BEFORE overriding the entries it actually implements. */
void device_init(device *self);
void device_deinit(device *self);

#endif /* DEVICE_H */
