#include "iface/io_xfer.h"

/* Driver/ISR-side helper: call when a transfer finishes. Signals the sync
 * waiter (if any) via the semaphore and invokes the completion callback (if
 * any). Safe to call from interrupt context. The transfer API itself
 * (stream_device_transfer_sync / _async) lives in iface/stream_device.c. */
void io_xfer_complete(io_xfer_t *xfer, int status)
{
    if (!xfer) return;
    xfer->status = status;
    osal_sem_give(&xfer->sem);                 /* wake a sync waiter, if any */
    if (xfer->callback) xfer->callback(xfer);  /* async completion path */
}
