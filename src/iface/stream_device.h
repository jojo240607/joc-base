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
 *
 * LOW-LEVEL TRANSFER ENGINE (polling / interrupt / DMA)
 * ------------------------------------------------------
 * A stream can be driven three ways at the silicon level. The synchronous
 * read()/write() contract covers ALL THREE — the driver picks the engine
 * internally and the upper layer only chooses WHICH one via STREAM_IOCTL_SET_MODE:
 *   POLL — CPU spins until the transfer completes (no IRQ/DMA).
 *   IRQ  — interrupt-driven: the ISR fills a ring / raises a semaphore and
 *          read()/write() block until it completes (non-blocking HW, blocking API).
 *   DMA  — a DMA controller moves the data; read()/write() block on the
 *          DMA-complete interrupt. A driver without a DMA engine returns -ENOSYS.
 * This keeps the interface small while making the three classic MCU engines
 * first-class. True non-blocking (callback) transfers are a SEPARATE, additive
 * extension (submit_read / submit_write + completion callback) and are NOT part
 * of this vtable, so every stream driver is not forced to implement async.
 */
typedef enum {
    STREAM_MODE_POLL = 0,   /* CPU spins until done */
    STREAM_MODE_IRQ,        /* interrupt-driven, blocking API */
    STREAM_MODE_DMA,        /* DMA engine, blocking API (driver may return -ENOSYS) */
} stream_xfer_mode_t;

/* ioctl commands understood by EVERY stream device (passed via device_ioctl) */
#define STREAM_IOCTL_SET_MODE  0xF0   /* arg: const stream_xfer_mode_t* */
#define STREAM_IOCTL_GET_MODE  0xF1   /* arg: stream_xfer_mode_t* */

typedef struct _stream_device stream_device;

/* Forward declaration of the unified transfer descriptor (iface/io_xfer.h).
 * Named here so the vtable can reference it WITHOUT pulling in io_xfer.h
 * (which in turn includes this header) — that would be a circular include. */
typedef struct io_xfer io_xfer_t;

struct stream_deviceVtable {
    int (*read)(stream_device *self, void *buf, size_t len);
    int (*write)(stream_device *self, const void *buf, size_t len);
    int (*flush)(stream_device *self);
    /* frame/message variants for CAN / USB / ETH (optional; meta = id/endpoint) */
    int (*read_frame)(stream_device *self, void *buf, size_t len, void *meta);
    int (*write_frame)(stream_device *self, const void *buf, size_t len, const void *meta);
    /* Optional async START: begin the transfer described by `xfer` and return
     * immediately (IRQ/DMA) or complete it inline (POLL, calling
     * io_xfer_complete before returning). When the transfer finishes the driver
     * calls io_xfer_complete(). NULL = not supported: io_transfer_sync falls
     * back to a read/write loop and io_transfer_async returns -1. */
    int (*submit)(stream_device *self, io_xfer_t *xfer);
};

struct _stream_device {
    device parent;                         /* IS-A device */
    const struct stream_deviceVtable *vtable;
    stream_xfer_mode_t mode;               /* selected engine (POLL/IRQ/DMA) */
};

/* upcast (subclass -> device, free pointer cast) */
static inline device *stream_device_to_device(stream_device *s) { return &s->parent; }

/* downcast (device -> subclass, requires class match, else NULL) */
static inline stream_device *device_as_stream(device *d)
    { return (d && d->class == DEVICE_CLASS_STREAM) ? (stream_device *)d : NULL; }

#endif /* STREAM_DEVICE_H */
