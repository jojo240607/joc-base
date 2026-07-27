#ifndef STREAM_DEVICE_H
#define STREAM_DEVICE_H

#include "iface/device.h"
#include "common/ringbuffer.h"   /* every stream device gets a ready-to-use RX ring buffer */
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
    /* DMA + IDLE-line: a CIRCULAR RX DMA keeps draining DR into a ring; the
     * USART IDLE interrupt (bus idle >1 byte-time) marks the END of a
     * variable-length frame. The IDLE ISR copies the received chunk into the
     * device RX ring so read()/getc() stay non-blocking and frame-length
     * agnostic. TX still uses the IRQ state machine. This is the recommended
     * default for a UART console: zero per-byte RX ISR overhead. */
    STREAM_MODE_DMA_IDLE,
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
     * calls io_xfer_complete(). NULL = not supported: stream_device_transfer_sync
     * falls back to a read/write loop and stream_device_transfer_async returns -1. */
    int (*submit)(stream_device *self, io_xfer_t *xfer);
    /* Unified synchronous / asynchronous transfer — the OOC entry points. These
     * normally point at the framework defaults stream_device_default_transfer_sync
     * / _async (which internally use submit or fall back to read/write). A driver
     * MAY override them (e.g. a native DMA engine). Callers reach them via the
     * inline stream_device_transfer_sync / _async wrappers, never as bare fns. */
    int (*transfer_sync)(stream_device *self, io_xfer_t *xfer);
    int (*transfer_async)(stream_device *self, io_xfer_t *xfer);
};

struct _stream_device {
    device parent;                         /* IS-A device */
    const struct stream_deviceVtable *vtable;
    stream_xfer_mode_t mode;               /* selected engine (POLL/IRQ/DMA) */
    /* RX ring buffer handle. A stream driver that needs an RX ring attaches one
     * via stream_device_init_ringbuffer() (which heap-allocates a ringbuffer
     * object backed by `buf`); drivers that don't need one (e.g. ADC) leave this
     * NULL and pay only a single pointer instead of a full ringbuffer struct.
     * Access it via stream_device_get_ringbuffer() (NULL-safe). */
    ringbuffer *rx_rb;
};

/* Attach an RX ring buffer to this stream device. Allocates a ringbuffer object
 * (heap) backed by `buf` (external storage — NOT freed by the ring; pass NULL to
 * let the ring allocate its own). Call once at open() time for devices that need
 * an RX ring. Re-attaching frees any previously attached ring first. */
void stream_device_init_ringbuffer(stream_device *self, uint8_t *buf, size_t size);

/* Free the attached RX ring buffer (if any). Call from the device's destroy path
 * so the heap object is released. Safe to call when no ring was attached. */
void stream_device_free_ringbuffer(stream_device *self);

/* Access the attached RX ring buffer (e.g. to push from an ISR / pop in read).
 * Returns NULL if `self` is NULL or no ring is attached — callers MUST NULL-check
 * before use. The ring is usable only after stream_device_init_ringbuffer(). */
ringbuffer *stream_device_get_ringbuffer(stream_device *self);

/* upcast (subclass -> device, free pointer cast) */
static inline device *stream_device_to_device(stream_device *s) { return &s->parent; }

/* downcast (device -> subclass, requires class match, else NULL) */
static inline stream_device *device_as_stream(device *d)
    { return (d && d->class == DEVICE_CLASS_STREAM) ? (stream_device *)d : NULL; }

/* Framework default implementation of the unified transfer API, shared by every
 * stream driver. A driver's vtable normally points transfer_sync / transfer_async
 * at these; a driver MAY override them for a specialized engine. */
int stream_device_default_transfer_sync (stream_device *self, io_xfer_t *xfer);
int stream_device_default_transfer_async(stream_device *self, io_xfer_t *xfer);

/* OOC entry points: dispatch the transfer THROUGH the object's vtable
 * (polymorphic). If a driver left the slot NULL we fall back to the shared
 * default so behavior stays correct (e.g. the read/write loop for drivers
 * without submit). This is how callers should always invoke the transfer API —
 * via the stream_device object, never a bare module-level function. */
static inline int stream_device_transfer_sync(stream_device *self, io_xfer_t *xfer)
{
    if (!self || !self->vtable) return -1;
    if (self->vtable->transfer_sync)
        return self->vtable->transfer_sync(self, xfer);
    return stream_device_default_transfer_sync(self, xfer);
}
static inline int stream_device_transfer_async(stream_device *self, io_xfer_t *xfer)
{
    if (!self || !self->vtable) return -1;
    if (self->vtable->transfer_async)
        return self->vtable->transfer_async(self, xfer);
    return stream_device_default_transfer_async(self, xfer);
}

#endif /* STREAM_DEVICE_H */
