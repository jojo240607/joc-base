#include "uart.h"
#include "iface/io_xfer.h"            /* io_xfer_t, io_xfer_complete (async API) */
#include "osal/osal.h"               /* osal_sem_wait / _init (TX serialization) */
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */

static uart *g_console = NULL;

/* virtual implementations dispatched through the unified device vtable */
static int uart_dev_open(device *self);
static int uart_dev_close(device *self);
static int uart_dev_read(device *self, void *buf, size_t len);
static int uart_dev_write(device *self, const void *buf, size_t len);
static int uart_dev_ioctl(device *self, int cmd, void *arg);

/* subclass vtable (defined below; forward-declared so uart_init can reference it) */
static const struct stream_deviceVtable uart_stream_vtable;
/* async transfer helper (defined below) */
static int uart_stream_submit(stream_device *self, io_xfer_t *xfer);
/* TX state machine (defined below) */
static void uart_tx_isr(uart *u);
static int uart_tx_blocking(uart *u, const char *s, size_t len);

/* public methods — `static`, reachable ONLY through self->fun-> */
static void uart_set_baudrate(uart *self, uint32_t baud);
static char uart_getc(uart *self);

/* RX ring-buffer + ISR helpers (defined below; declared here so uart_getc can
 * call uart_rx_getc before its definition). */
static void uart_rx_putc(uart *self, char c);
static char uart_rx_getc(uart *self);
static void uart_isr(void *ctx);

const struct uartFun uart_fun = {
    .destroy      = uart_destroy,
    .init         = uart_init,
    .deinit       = uart_deinit,
    .set_baudrate = uart_set_baudrate,
    .getc         = uart_getc,
};

/* one shared vtable for the whole UART class — assigned by uart_init() */
static const struct deviceVtable uart_dev_vtable = {
    .open  = uart_dev_open,
    .close = uart_dev_close,
    .read  = uart_dev_read,
    .write = uart_dev_write,
    .ioctl = uart_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *uart_create(const void *config)
{
    const uart_config_t *c = (const uart_config_t *)config;
    uart *self = (uart *)malloc(sizeof(uart));
    if (!self) return NULL;
    memset(self, 0, sizeof(uart));
    self->hal = uart_hal_create(c->periph, c->baud);
    if (!self->hal) { free(self); return NULL; }   /* #9: HAL alloc failure */
    self->parent.parent.type = DEVICE_TYPE_UART;    /* driver sets its own class */
    self->parent.parent.name = c->name;             /* driver sets its own name */
    self->tx_signal = c->tx_signal;          /* cache names for pinmux claim at open() */
    self->rx_signal = c->rx_signal;
    uart_init(self);
    if (c->is_console) uart_set_console(self);
    return (device *)self;
}

void uart_destroy(uart *self)
{
    if (!self) return;
    uart_deinit(self);
    uart_hal_destroy(self->hal);   /* mirror create: free the HAL handle */
    free(self);
}

void uart_init(uart *self)
{
    if (!self) return;
    self->parent.parent.vtable = &uart_dev_vtable;    /* base device vtable */
    self->parent.vtable        = &uart_stream_vtable; /* stream-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_UART;
    self->parent.parent.class  = DEVICE_CLASS_STREAM;
    self->parent.mode          = STREAM_MODE_IRQ;     /* RX is interrupt-driven */
    self->fun = &uart_fun;
    osal_sem_init(&self->tx_idle, 1);  /* line starts free; the TXE ISR gives it back */
    /* hardware bring-up is deferred to open() (see uart_dev_open) */
}

void uart_deinit(uart *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
}

static void uart_set_baudrate(uart *self, uint32_t baud)
{
    if (!self) return;
    uart_hal_set_baudrate(self->hal, baud);
}

void uart_set_console(uart *self)
{
    g_console = self;
}

void uart_console_putc(char c)
{
    if (!g_console) return;
    /* In IRQ mode route through the TX state machine so printf output is
     * serialized with stream writes on the same UART (no wire corruption). */
    if (g_console->parent.mode == STREAM_MODE_IRQ)
        uart_tx_blocking(g_console, &c, 1);
    else
        uart_hal_putc(g_console->hal, c);
}

static char uart_getc(uart *self)
{
    /* drain the RX ring buffer (filled by the receive ISR) */
    return self ? uart_rx_getc(self) : 0;
}

/* --- RX ring buffer + interrupt ISR (platform-independent irq framework) --- */

/* Push one received byte into the embedded RX ring buffer (called from
 * interrupt context). Drops on overflow (ring buffer is in no-overwrite mode). */
static void uart_rx_putc(uart *self, char c)
{
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)self);
    if (rb) rb->fun->put(rb, (uint8_t)c);
}

/* Pop one byte, blocking until the ISR delivers one (thread context). */
static char uart_rx_getc(uart *self)
{
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)self);
    uint8_t c = 0;
    while (!rb || rb->fun->is_empty(rb)) { }   /* wait for the ISR to fill */
    rb->fun->get(rb, &c);
    return (char)c;
}

/* The receive ISR callback. Registered with the framework via irq_register()
 * (see uart_dev_open); `ctx` is the uart instance. Reading DR clears RXNE.
 * After pushing the byte into the ring, if an asynchronous read is in progress
 * (async_rx != NULL) we drain the ring straight into that xfer and signal
 * completion once it is full — this is the genuine IRQ-driven async path. */
static void uart_isr(void *ctx)
{
    uart *u = (uart *)ctx;
    /* RX: only act when a character is actually pending, so reading DR (which
     * clears RXNE) is never done spuriously. */
    if (uart_hal_rx_pending(u->hal)) {
        uart_rx_putc(u, uart_hal_read_dr(u->hal));
        if (u->async_rx) {
            io_xfer_t *x = u->async_rx;
            ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
            uint8_t c;
            while (x->done < x->len && rb && rb->fun->get(rb, &c) == 0)
                ((char *)x->buf)[x->done++] = (char)c;
            if (x->done >= x->len) {        /* transfer complete */
                u->async_rx = NULL;
                io_xfer_complete(x, 0);      /* wake sync waiter + invoke callback */
            }
        }
    }
    /* TX: drain the in-progress transfer (blocking write or async submit). */
    if (uart_hal_tx_ready(u->hal)) {
        uart_tx_isr(u);
    }
}

/* TXE ISR: send the next byte of the in-progress transfer. When the last byte
 * leaves, disable the TXE interrupt and release the line (tx_idle); for a
 * blocking writer also signal the per-transfer completion semaphore, for an
 * async writer signal the xfer instead. */
static void uart_tx_isr(uart *u)
{
    if (u->tx_rem > 0) {
        uart_hal_write_dr(u->hal, *u->tx_ptr++);
        u->tx_rem--;
        if (u->tx_rem == 0) {
            uart_hal_disable_tx_irq(u->hal);
            if (u->async_tx) {
                io_xfer_t *x = u->async_tx;
                u->async_tx = NULL;
                x->done = x->len;
                osal_sem_give(&u->tx_idle);   /* line free for the next TX */
                io_xfer_complete(x, 0);        /* wake async waiter + callback */
            } else {
                osal_sem_give(&u->tx_idle);
                osal_sem_give(&u->tx_done_sem);/* wake the blocking writer */
            }
        }
    } else {
        uart_hal_disable_tx_irq(u->hal);       /* nothing pending; silence */
    }
}

/* Blocking transmit through the TX state machine. Serialized with every other
 * TX (printf, async submit) via tx_idle, so they can never interleave on the
 * wire. Used by uart_stream_write (IRQ mode) and uart_console_putc. */
static int uart_tx_blocking(uart *u, const char *s, size_t len)
{
    if (len == 0) return 0;
    osal_sem_wait(&u->tx_idle);          /* wait for the line to be free */
    u->async_tx = NULL;
    u->tx_ptr   = s;
    u->tx_rem   = len;
    osal_sem_init(&u->tx_done_sem, 0);
    uart_hal_enable_tx_irq(u->hal);      /* TXE ISR drains tx_rem */
    osal_sem_wait(&u->tx_done_sem);      /* block until the last byte is sent */
    return (int)len;
}

/* --- unified device-interface virtual implementations --- */

static int uart_dev_open(device *self)
{
    uart *u = (uart *)self;

    /* Claim + program the TX/RX pins through the pinmux BEFORE touching any
     * GPIO register. The board supplied a SIGNAL NAME (e.g. "USART1_TX_PA9");
     * pinmux_hal_resolve() turns it into the exact (port, pin, af). Because the
     * AF database has NO duplicate names (suffixes guarantee uniqueness), the
     * name maps to exactly one pad — no "first match wins" ambiguity. If a pin
     * is already owned by another device the request fails and we refuse to
     * configure it (and roll back any pin already claimed) — that is the
     * arbitrator. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        pinmux_port_t port; uint8_t pin, af;
        pinmux_pin_cfg_t cfg = { .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };

        /* TX */
        if (!pinmux_hal_resolve(u->tx_signal, &port, &pin, &af)) {
            printf("[uart] %s: unknown TX signal \"%s\"\r\n", u->parent.parent.name, u->tx_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            printf("[uart] %s: TX pin P%c%d CONFLICT — refused\r\n",
                   u->parent.parent.name, 'A' + port, pin);
            return -2;                       /* conflict: do NOT configure */
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);

        /* RX */
        if (!pinmux_hal_resolve(u->rx_signal, &port, &pin, &af)) {
            printf("[uart] %s: unknown RX signal \"%s\"\r\n", u->parent.parent.name, u->rx_signal);
            pm->fun->release_owner(pm, u->parent.parent.name);   /* roll back TX */
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            printf("[uart] %s: RX pin P%c%d CONFLICT — refused\r\n",
                   u->parent.parent.name, 'A' + port, pin);
            pm->fun->release_owner(pm, u->parent.parent.name);   /* roll back TX */
            return -2;
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);
    }

    uart_hal_init(u->hal);

    /* Attach our RX storage to the embedded ring buffer (common/ringbuffer) so
     * the receive ISR can push bytes and read()/getc() can drain them. */
    stream_device_init_ringbuffer((stream_device *)u, (uint8_t *)u->rx_buf, UART_RX_BUF_SIZE);

    /* Wire the RX interrupt through the PLATFORM-INDEPENDENT irq framework.
     * The driver registers a callback + its own context; the HAL supplies the
     * chip IRQ number (uart_hal_irq_id), so the driver never names a Cortex-M /
     * STM32 interrupt directly. The ISR (uart_isr) reads DR and fills the ring
     * buffer; read()/getc() then drain it. */
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_register(id, uart_isr, u);          /* combined RX+TX ISR */
    irq_set_priority(id, 0);
    if (u->parent.mode == STREAM_MODE_IRQ)
        uart_hal_enable_rx_irq(u->hal);     /* RX ISR only in IRQ mode */
    irq_enable(id);
    return 0;
}

static int uart_dev_close(device *self)
{
    uart *u = (uart *)self;
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_disable(id);                    /* stop the ISR first */
    uart_hal_disable_rx_irq(u->hal);
    uart_hal_disable_tx_irq(u->hal);
    irq_register(id, NULL, NULL);       /* uninstall the callback */
    uart_hal_deinit(u->hal);
    return 0;
}

/* stream-class ops — the REAL implementations; the base deviceVtable forwards
 * here so there is a single source of truth for the data path. The transfer
 * engine is chosen by self->parent.mode (POLL/IRQ/DMA, see stream_device.h). */
static int uart_stream_read(stream_device *self, void *buf, size_t len)
{
    uart *u = (uart *)self;
    if (len < 1 || !buf) return -1;
    if (u->parent.mode == STREAM_MODE_DMA) return -1;   /* no DMA engine here */
    if (u->parent.mode == STREAM_MODE_POLL) {
        while (!uart_hal_rx_pending(u->hal)) { }        /* busy-wait, no ISR */
        *(char *)buf = uart_hal_read_dr(u->hal);
        return 1;
    }
    *(char *)buf = uart_rx_getc(u);      /* IRQ: ISR-fed ring — spin until byte */
    return 1;
}

static int uart_stream_write(stream_device *self, const void *buf, size_t len)
{
    uart *u = (uart *)self;
    const char *s = (const char *)buf;
    if (!buf) return -1;
    if (u->parent.mode == STREAM_MODE_DMA) return -1;   /* no DMA engine here */
    if (u->parent.mode == STREAM_MODE_IRQ)
        return uart_tx_blocking(u, s, len);             /* interrupt-driven TX */
    for (size_t i = 0; i < len; i++)
        uart_hal_putc(u->hal, s[i]);    /* polling TX */
    return (int)len;
}

static int uart_stream_flush(stream_device *self)
    { (void)self; return 0; }
static int uart_stream_read_frame(stream_device *self, void *buf, size_t len, void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }
static int uart_stream_write_frame(stream_device *self, const void *buf, size_t len, const void *meta)
    { (void)self; (void)buf; (void)len; (void)meta; return -1; }

static const struct stream_deviceVtable uart_stream_vtable = {
    .read        = uart_stream_read,
    .write       = uart_stream_write,
    .flush       = uart_stream_flush,
    .read_frame  = uart_stream_read_frame,
    .write_frame = uart_stream_write_frame,
    .submit      = uart_stream_submit,
    .transfer_sync  = stream_device_default_transfer_sync,
    .transfer_async = stream_device_default_transfer_async,
};

/* async START (stream vtable). Begins a transfer and returns immediately; the
 * driver later calls io_xfer_complete() (from the ISR for reads, or inline for
 * the polling TX below). This is the single hook the framework needs to support
 * both stream_device_transfer_sync (block on the completion semaphore) and
 * io_transfer_async (return at once, callback on completion). */
static int uart_stream_submit(stream_device *self, io_xfer_t *xfer)
{
    uart *u = (uart *)self;
    if (!xfer || !xfer->buf) return -1;
    if (u->parent.mode == STREAM_MODE_DMA) return -1;   /* no DMA engine here */

    if (xfer->dir == IO_XFER_DIR_WRITE) {
        if (u->parent.mode == STREAM_MODE_POLL) {
            const char *s = (const char *)xfer->buf;
            for (size_t i = 0; i < xfer->len; i++)
                uart_hal_putc(u->hal, s[i]);    /* polling TX */
            xfer->done = xfer->len;
            io_xfer_complete(xfer, 0);          /* signal + callback (inline) */
            return 0;
        }
        /* IRQ: drive TX from the TXE ISR (serialized via tx_idle). The ISR
         * calls io_xfer_complete() when the last byte leaves. */
        osal_sem_wait(&u->tx_idle);
        u->async_tx = xfer;
        u->tx_ptr   = (const char *)xfer->buf;
        u->tx_rem   = xfer->len;
        xfer->done  = 0;
        if (xfer->len == 0) {
            u->async_tx = NULL;
            osal_sem_give(&u->tx_idle);
            io_xfer_complete(xfer, 0);
            return 0;
        }
        uart_hal_enable_tx_irq(u->hal);
        return 0;                               /* ISR completes */
    }

    /* READ */
    if (u->parent.mode == STREAM_MODE_POLL) {
        while (xfer->done < xfer->len) {
            while (!uart_hal_rx_pending(u->hal)) { }
            ((char *)xfer->buf)[xfer->done++] = uart_hal_read_dr(u->hal);
        }
        io_xfer_complete(xfer, 0);
        return 0;
    }
    /* IRQ: hand off to the RX ISR. Drain any bytes already buffered, then let
     * uart_isr() finish the rest and call io_xfer_complete(). */
    u->async_rx = xfer;
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
    uint8_t c;
    while (xfer->done < xfer->len && rb && rb->fun->get(rb, &c) == 0)
        ((char *)xfer->buf)[xfer->done++] = (char)c;
    if (xfer->done >= xfer->len) {          /* all available already */
        u->async_rx = NULL;
        io_xfer_complete(xfer, 0);
    }
    return 0;                               /* started; ISR completes if not done */
}

/* base device-interface ops forward to the stream-class vtable */
static int uart_dev_read(device *self, void *buf, size_t len)
    { return uart_stream_read((stream_device *)self, buf, len); }
static int uart_dev_write(device *self, const void *buf, size_t len)
    { return uart_stream_write((stream_device *)self, buf, len); }

static int uart_dev_ioctl(device *self, int cmd, void *arg)
{
    uart *u = (uart *)self;
    switch (cmd) {
    case UART_IOCTL_SET_BAUDRATE:
        if (!arg) return -1;
        uart_set_baudrate(u, *(const uint32_t *)arg);
        return 0;
    case UART_IOCTL_GET_BAUDRATE:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_baudrate(u->hal);
        return 0;
    case UART_IOCTL_GET_BRR:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_brr(u->hal);
        return 0;
    case UART_IOCTL_GET_CR1:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_cr1(u->hal);
        return 0;
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        if (m == STREAM_MODE_DMA) return -1;   /* no DMA engine here */
        u->parent.mode = m;
        if (m == STREAM_MODE_IRQ) uart_hal_enable_rx_irq(u->hal);
        else uart_hal_disable_rx_irq(u->hal);
        return 0;
    }
    case STREAM_IOCTL_GET_MODE:
        if (!arg) return -1;
        *(stream_xfer_mode_t *)arg = u->parent.mode;
        return 0;
    default:
        return -1;
    }
}
