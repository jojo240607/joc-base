#include "iface/stream_device.h"
#include "iface/io_xfer.h"   /* io_xfer_t, io_xfer_complete */
#include "osal/osal.h"
#include <stddef.h>

/*
 * Framework default implementation of the unified sync/async transfer API.
 *
 * These are the "framework top-level" the driver author asked for: a single
 * pair of calls that works on ANY stream_device regardless of which low-level
 * engine (POLL / IRQ / DMA) the driver uses internally. A driver's vtable
 * normally points its transfer_sync / transfer_async slots at these; a driver
 * MAY override them for a specialized engine (e.g. a native DMA path). They are
 * reached through the inline stream_device_transfer_sync / _async wrappers, so
 * callers always go through the object (polymorphic), never a bare module fn.
 *
 * How the two map onto the three engines (driver-internal):
 *   POLL    — sync: start + spin polling. async: the driver does it inline then
 *                    calls io_xfer_complete() (callback fires before async
 *                    returns — i.e. it "completes synchronously").
 *   IRQ/DMA — sync: start + osal_sem_wait() on the completion semaphore.
 *             async: start + return; the ISR later calls io_xfer_complete(),
 *                    which gives the semaphore (wakes any sync waiter) AND
 *                    invokes the callback. The semaphore is the ONLY thing the
 *                    RTOS port must swap — driver code is untouched.
 */
int stream_device_default_transfer_sync(stream_device *self, io_xfer_t *xfer)
{
    if (!self || !self->vtable || !xfer) return -1;
    if (xfer->dir != IO_XFER_DIR_READ && xfer->dir != IO_XFER_DIR_WRITE) return -1;

    osal_sem_init(&xfer->sem, 0);
    xfer->done   = 0;
    xfer->status = 0;

    /* Path A: driver exposes a start/submit op (true async-capable engine). */
    if (self->vtable->submit) {
        int r = self->vtable->submit(self, xfer);
        if (r < 0) return r;                   /* start failed */
        osal_sem_wait(&xfer->sem);             /* block until completion fires */
        return (xfer->status < 0) ? xfer->status : (int)xfer->done;
    }

    /* Path B: fallback for drivers without submit — drive read/write in a loop
     * (covers POLL and blocking IRQ/DMA whose read()/write() already block). */
    if (xfer->dir == IO_XFER_DIR_READ) {
        while (xfer->done < xfer->len) {
            int n = self->vtable->read(self, (char *)xfer->buf + xfer->done,
                                       xfer->len - xfer->done);
            if (n < 0) { xfer->status = n; return n; }
            xfer->done += (size_t)n;
        }
    } else {
        while (xfer->done < xfer->len) {
            int n = self->vtable->write(self, (const char *)xfer->buf + xfer->done,
                                        xfer->len - xfer->done);
            if (n < 0) { xfer->status = n; return n; }
            xfer->done += (size_t)n;
        }
    }
    return (int)xfer->done;
}

int stream_device_default_transfer_async(stream_device *self, io_xfer_t *xfer)
{
    if (!self || !self->vtable || !xfer) return -1;
    if (xfer->dir != IO_XFER_DIR_READ && xfer->dir != IO_XFER_DIR_WRITE) return -1;

    osal_sem_init(&xfer->sem, 0);
    xfer->done   = 0;
    xfer->status = 0;

    /* Async REQUIRES a submit op: the driver must be able to start a transfer
     * that completes later (in its ISR). Drivers without submit return -1. */
    if (!self->vtable->submit) return -1;
    return self->vtable->submit(self, xfer);     /* start; returns immediately */
}

/* --- embedded RX ring buffer helpers (see stream_device.h) --- */ 

void stream_device_init_ringbuffer(stream_device *self, uint8_t *buf, size_t size)
{
    if (!self || size < 2) return;
    /* Allocate the ringbuffer object on the heap, backed by the caller's storage
     * (owns_buf = 0, so only the small ringbuffer struct is heap; the byte store
     * stays in the driver). Free any previously attached ring first. */
    stream_device_free_ringbuffer(self);
    ringbuffer_config_t cfg = { .buf = buf, .size = size, .overwrite = 0 };
    self->rx_rb = ringbuffer_create(&cfg);
}

void stream_device_free_ringbuffer(stream_device *self)
{
    if (self && self->rx_rb) {
        ringbuffer_destroy(self->rx_rb);   /* frees the heap object (not buf) */
        self->rx_rb = NULL;
    }
}

ringbuffer *stream_device_get_ringbuffer(stream_device *self)
{
    return self ? self->rx_rb : NULL;
}
