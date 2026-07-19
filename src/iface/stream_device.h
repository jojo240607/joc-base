#ifndef STREAM_DEVICE_H
#define STREAM_DEVICE_H

#include "iface/device.h"
#include <stddef.h>

/*
 * 数据流设备 (Data-stream) base class — a subclass of `device`.
 *
 * Covers UART, SPI, I2C, I2S, ADC, DAC, CAN, Ethernet, USB, LCD: peripherals
 * that move a CONTINUOUS stream of bytes (or frames). The base `device` already
 * gives open/close/ioctl; this class adds the bulk-transfer ops its members
 * actually need:
 *   - read / write / flush      — byte streams (UART/SPI/I2C/I2S/ADC/DAC/LCD)
 *   - read_frame / write_frame  — message/frame variants for CAN/USB/ETH, where
 *                                 each unit carries id/endpoint metadata (meta).
 *
 * INHERITANCE (C style): embed `device parent;` as the FIRST member, so a
 * stream_device* and its device* alias the same address (upcast = free cast).
 * To downcast device* -> stream_device*, check the class tag (device.class)
 * then cast (see device_as_stream). The device_manager stays 100% generic.
 */

typedef struct _stream_device stream_device;

struct stream_deviceVtable {
    int (*read)(stream_device *self, void *buf, size_t len);
    int (*write)(stream_device *self, const void *buf, size_t len);
    int (*flush)(stream_device *self);
    /* frame/message variants for CAN / USB / ETH (optional; meta = id/endpoint) */
    int (*read_frame)(stream_device *self, void *buf, size_t len, void *meta);
    int (*write_frame)(stream_device *self, const void *buf, size_t len, const void *meta);
};

struct _stream_device {
    device parent;                         /* IS-A device */
    const struct stream_deviceVtable *vtable;
};

/* upcast (subclass -> device, free pointer cast) */
static inline device *stream_device_to_device(stream_device *s) { return &s->parent; }

/* downcast (device -> subclass, requires class match, else NULL) */
static inline stream_device *device_as_stream(device *d)
    { return (d && d->class == DEVICE_CLASS_STREAM) ? (stream_device *)d : NULL; }

#endif /* STREAM_DEVICE_H */
