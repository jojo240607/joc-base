#ifndef IO_XFER_H
#define IO_XFER_H

#include "iface/stream_device.h"
#include "osal/osal.h"
#include <stddef.h>

/*
 * Unified synchronous / asynchronous transfer descriptor + driver/ISR completion
 * helper for stream devices.
 *
 * The transfer API ITSELF lives in the stream_device interface
 * (iface/stream_device.h): stream_device_transfer_sync / _async are reached
 * through the stream_device object (polymorphic). This file only defines the
 * transfer descriptor (io_xfer_t) and the driver/ISR-side completion helper
 * io_xfer_complete() that a driver calls when a transfer finishes.
 *
 * How sync/async map onto the three engines (driver-internal):
 *   POLL    — sync: start + spin polling. async: the driver does it inline then
 *                    calls io_xfer_complete() (callback fires before async
 *                    returns — i.e. it "completes synchronously").
 *   IRQ/DMA — sync: start + osal_sem_wait() on the completion semaphore.
 *             async: start + return; the ISR later calls io_xfer_complete(),
 *                    which gives the semaphore (wakes any sync waiter) AND
 *                    invokes the callback. The semaphore is the ONLY thing the
 *                    RTOS port must swap — driver code is untouched.
 *
 * A driver opts into async by filling stream_deviceVtable.submit. Drivers that
 * do not implement it still work: stream_device_transfer_sync falls back to a
 * read/write loop, and stream_device_transfer_async returns -1 (not supported).
 */
typedef enum {
    IO_XFER_DIR_READ  = 0,
    IO_XFER_DIR_WRITE = 1,
} io_xfer_dir_t;

/* forward declaration — full definition below. stream_device.h also
 * forward-declares this type so it can name it in its vtable without a cycle. */
typedef struct io_xfer io_xfer_t;

struct io_xfer {
    void          *buf;       /* data buffer (read: dest, write: src) */
    size_t         len;       /* requested length, in bytes */
    size_t         done;      /* bytes transferred so far (driver updates) */
    int            status;    /* completion status: 0 ok, <0 error (driver sets) */
    io_xfer_dir_t  dir;       /* IO_XFER_DIR_READ / IO_XFER_DIR_WRITE */
    void         (*callback)(io_xfer_t *xfer);  /* async completion cb (NULL = none) */
    void          *arg;       /* user context handed to callback */
    osal_sem_t     sem;       /* internal completion flag (signaled by driver) */
};

/* Driver/ISR-side helper: call when a transfer finishes. Signals the sync
 * waiter (if any) via the semaphore and invokes the completion callback (if
 * any). Safe to call from interrupt context. The transfer API itself
 * (stream_device_transfer_sync / _async) is declared in iface/stream_device.h. */
void io_xfer_complete(io_xfer_t *xfer, int status);

#endif /* IO_XFER_H */
