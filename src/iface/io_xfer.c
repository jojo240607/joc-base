#include "iface/io_xfer.h"

void io_xfer_complete(io_xfer_t *xfer, int status)
{
    if (!xfer) return;
    xfer->status = status;
    osal_sem_give(&xfer->sem);                 /* wake a sync waiter, if any */
    if (xfer->callback) xfer->callback(xfer);  /* async completion path */
}

int io_transfer_sync(stream_device *dev, io_xfer_t *xfer)
{
    if (!dev || !dev->vtable || !xfer) return -1;
    if (xfer->dir != IO_XFER_DIR_READ && xfer->dir != IO_XFER_DIR_WRITE) return -1;

    osal_sem_init(&xfer->sem, 0);
    xfer->done   = 0;
    xfer->status = 0;

    /* Path A: driver exposes a start/submit op (true async-capable engine). */
    if (dev->vtable->submit) {
        int r = dev->vtable->submit(dev, xfer);
        if (r < 0) return r;                   /* start failed */
        osal_sem_wait(&xfer->sem);             /* block until completion fires */
        return (xfer->status < 0) ? xfer->status : (int)xfer->done;
    }

    /* Path B: fallback for drivers without submit — drive read/write in a loop
     * (covers POLL and blocking IRQ/DMA whose read()/write() already block). */
    if (xfer->dir == IO_XFER_DIR_READ) {
        while (xfer->done < xfer->len) {
            int n = dev->vtable->read(dev, (char *)xfer->buf + xfer->done,
                                      xfer->len - xfer->done);
            if (n < 0) { xfer->status = n; return n; }
            xfer->done += (size_t)n;
        }
    } else {
        while (xfer->done < xfer->len) {
            int n = dev->vtable->write(dev, (const char *)xfer->buf + xfer->done,
                                       xfer->len - xfer->done);
            if (n < 0) { xfer->status = n; return n; }
            xfer->done += (size_t)n;
        }
    }
    return (int)xfer->done;
}

int io_transfer_async(stream_device *dev, io_xfer_t *xfer)
{
    if (!dev || !dev->vtable || !xfer) return -1;
    if (xfer->dir != IO_XFER_DIR_READ && xfer->dir != IO_XFER_DIR_WRITE) return -1;

    osal_sem_init(&xfer->sem, 0);
    xfer->done   = 0;
    xfer->status = 0;

    /* Async REQUIRES a submit op: the driver must be able to start a transfer
     * that completes later (in its ISR). Drivers without submit return -1. */
    if (!dev->vtable->submit) return -1;
    return dev->vtable->submit(dev, xfer);     /* start; returns immediately */
}
