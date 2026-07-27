#include "uart.h"
#include "irq_manager.h"              /* centralized interrupt manager */
#include "iface/io_xfer.h"            /* io_xfer_t, io_xfer_complete (async API) */
#include "osal/osal.h"               /* osal_sem_wait / _init (TX serialization) */
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include "drv/dma.h"                  /* dma device + dma_hal_route (DMA engine) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */
#include "log/log.h"
#include "log/app_log.h"

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
    self->dma_tx_req = c->dma_tx_req;        /* cache DMA request IDs for open() */
    self->dma_rx_req = c->dma_rx_req;
    uart_init(self);
    if (c->is_console) uart_set_console(self);
    return (device *)self;
}

void uart_destroy(uart *self)
{
    if (!self) return;
    uart_deinit(self);
    stream_device_free_ringbuffer((stream_device *)self);  /* release RX ring (heap) */
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
    self->parent.mode          = STREAM_MODE_DMA_IDLE;/* RX: circular DMA + IDLE (zero per-byte ISR) */
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
    /* Route through the TX state machine in IRQ and DMA_IDLE modes so printf
     * output is serialized with stream writes on the same UART (no wire
     * corruption). Only the bulk STREAM_MODE_DMA path falls back to polling. */
    if (g_console->parent.mode == STREAM_MODE_IRQ ||
        g_console->parent.mode == STREAM_MODE_DMA_IDLE)
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

/* --- IDLE-line DMA RX (STREAM_MODE_DMA_IDLE) ---
 * A CIRCULAR RX DMA continuously drains DR into dma_bounce; the USART IDLE
 * interrupt (bus idle >1 byte-time) marks the END of a variable-length frame.
 * On IDLE we compute how many bytes the DMA has written since the last flush
 * (via NDTR), copy that chunk from the circular buffer into the RX ring, and
 * re-arm. This gives a zero per-byte-ISR RX path while keeping read()/getc()
 * non-blocking and frame-length agnostic. */

/* Arm the circular RX DMA for idle-line reception. */
static void uart_idle_dma_arm(uart *u)
{
    if (!u->dma_dev || !u->dma_rx) return;
    void *dr = uart_hal_get_dr_addr(u->hal);
    u->dma_idle_bufsize = UART_DMA_BOUNCE;
    /* P2M, PAR=DR, memory=dma_idle_buf, MINC, 8-bit, circular (start_circular).
     * A SEPARATE buffer from dma_bounce so TX DMA never clobbers RX data. */
    u->dma_dev->fun->config(u->dma_dev, u->dma_rx, dr, u->dma_idle_buf,
                            UART_DMA_BOUNCE, DMA_DATA_8, 0 /*periph_inc*/,
                            1 /*mem_inc*/, DMA_PRIO_MED);
    u->dma_dev->fun->start_circular(u->dma_dev, u->dma_rx);
    uart_hal_enable_rx_dma(u->hal);      /* DMAR: USART raises DMA requests */
    uart_hal_clear_idle(u->hal);         /* clear any stale IDLE (read SR then DR) */
    uart_hal_enable_idle_irq(u->hal);    /* IDLEIE: interrupt at frame end */
    u->dma_idle_total = 0;               /* NDTR-based running total */
}

/* Stop idle-line reception (disable IDLE + DMAR). The circular RX stream is also
 * paused (EN=0) so it does not linger in a CIRC/EN=1 state across the mode switch
 * — otherwise a later bulk uart_dma_read that re-programs the SAME stream could
 * inherit stale circular state. Re-arming (uart_idle_dma_arm) re-configs it. */
static void uart_idle_dma_disarm(uart *u)
{
    uart_hal_disable_idle_irq(u->hal);
    uart_hal_disable_rx_dma(u->hal);
    if (u->dma_dev && u->dma_rx)
        u->dma_dev->fun->stop(u->dma_dev, u->dma_rx);   /* EN=0, no TC/TE IRQ */
}

/* Copy the bytes the circular RX DMA has written since the last flush into the
 * RX ring. Uses the running NDTR total so wrap-around is handled correctly. */
static void uart_idle_flush(uart *u)
{
    uint32_t n   = u->dma_idle_bufsize;
    if (!n) return;
    uint32_t rem = u->dma_dev->fun->remaining(u->dma_dev, u->dma_rx); /* NDTR */
    uint32_t total = n - rem;             /* bytes written since arming (mod n) */
    int32_t d = (int32_t)total - (int32_t)u->dma_idle_total;
    if (d < 0) d += (int32_t)n;           /* wrapped at least once */
    uint32_t newb = (uint32_t)d;
    uint32_t src  = u->dma_idle_total % n;
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
    for (uint32_t i = 0; i < newb; i++)
        if (rb) rb->fun->put(rb, u->dma_idle_buf[(src + i) % n]);
    u->dma_idle_total = total;
}

/* Drain any bytes already buffered in the RX ring into an in-progress async
 * read, signalling completion when it is full. Shared by the IRQ and IDLE RX
 * paths (the producer is whichever engine is active). */
static void uart_rx_drain_async(uart *u)
{
    if (!u->async_rx) return;
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

/* The receive ISR callback. Registered with the framework via irq_register()
 * (see uart_dev_open); `ctx` is the uart instance. In STREAM_MODE_DMA_IDLE the
 * IDLE interrupt flushes the circular DMA chunk into the RX ring; in IRQ mode
 * each RXNE pushes a single byte. After either producer runs we drain the ring
 * into any in-progress async read. */
static void uart_isr(void *ctx)
{
    uart *u = (uart *)ctx;
    /* IDLE: a variable-length frame just ended — flush the DMA chunk to the ring. */
    if (u->parent.mode == STREAM_MODE_DMA_IDLE && uart_hal_idle_pending(u->hal)) {
        uart_idle_flush(u);
        uart_hal_clear_idle(u->hal);   /* read SR then DR to clear IDLE */
    }
    /* Overrun (ORE) FREEZES the receiver until it is cleared (read SR then DR).
     * If we don't clear it here, RXNE never re-asserts and the per-byte IRQ path
     * deadlocks. clear_errors() also consumes the held byte, so re-check RXNE
     * afterwards and push any fresh byte. */
    if (uart_hal_ore_pending(u->hal)) {
        uart_hal_clear_errors(u->hal);
    }
    /* RX: per-byte path (IRQ mode); reading DR clears RXNE. */
    if (uart_hal_rx_pending(u->hal)) {
        uart_rx_putc(u, uart_hal_read_dr(u->hal));
    }
    /* Drain the ring into any in-progress async read (either RX engine). */
    uart_rx_drain_async(u);
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

/* --- DMA engine (STREAM_MODE_DMA) ---
 * A UART TX/RX is hard-wired by the silicon to ONE specific DMA stream. We
 * resolve that stream once at open() (via dma_hal_route) and keep it reserved;
 * TX/RX just arm a transfer on it. The DMA ISR (in drv/dma.c) gives the stream's
 * done_sem on Transfer-Complete, so write/read block until the hardware finishes
 * — the blocking API contract is preserved. */

/* Resolve the TX/RX DMA routes for this UART and reserve the hard-wired streams
 * from the matching dma controller. Called once at open(); the streams stay
 * reserved until close(). Returns 0 if at least one direction resolved. */
static int uart_dma_acquire(uart *u)
{
    u->dma_dev = NULL; u->dma_tx = NULL; u->dma_rx = NULL;
    if (u->dma_tx_req == DMA_REQ_NONE && u->dma_rx_req == DMA_REQ_NONE)
        return 0;   /* this UART has no DMA configured */
    if (u->dma_tx_req != DMA_REQ_NONE) {
        dma_route_t rt = dma_hal_route(u->dma_tx_req);
        if (rt.name) {
            dma *dm = (dma *)device_manager_get(rt.name);
            if (dm) {
                dma_stream_t *s = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_M2P);
                if (s) { u->dma_dev = dm; u->dma_tx = s; }
                else log_printf(app_log(), LOG_DEBUG, "uart",
                       "%s: DMA TX stream busy\n", u->parent.parent.name);
            }
        }
    }
    if (u->dma_rx_req != DMA_REQ_NONE) {
        dma_route_t rt = dma_hal_route(u->dma_rx_req);
        if (rt.name) {
            dma *dm = (dma *)device_manager_get(rt.name);
            if (dm) {
                dma_stream_t *s = dm->fun->acquire(dm, rt.stream, rt.channel, DMA_DIR_P2M);
                if (s) { if (!u->dma_dev) u->dma_dev = dm; u->dma_rx = s; }
            }
        }
    }
    return (u->dma_tx || u->dma_rx) ? 0 : -1;
}

/* Release the reserved DMA streams (called at close). */
static void uart_dma_release(uart *u)
{
    uart_hal_disable_tx_dma(u->hal);
    uart_hal_disable_rx_dma(u->hal);
    if (u->dma_dev) {
        if (u->dma_tx) u->dma_dev->fun->free(u->dma_dev, u->dma_tx);
        if (u->dma_rx) u->dma_dev->fun->free(u->dma_dev, u->dma_rx);
    }
    u->dma_dev = NULL; u->dma_tx = NULL; u->dma_rx = NULL;
}

/* DMA transmit: program the reserved TX stream (M2P, PAR=DR, memory=src, MINC),
 * gate the USART onto the DMA (DMAT), arm it and block until Transfer-Complete.
 * Returns the number of bytes moved. The caller's buffer may live in CCM (e.g.
 * a stack array), which DMA cannot touch, so we copy it into the main-SRAM
 * bounce first and DMA from there. */
static int uart_dma_write(uart *u, const char *s, size_t len)
{
    if (!u->dma_dev || !u->dma_tx) return -1;
    void *dr = uart_hal_get_dr_addr(u->hal);
    const uint8_t *src = (const uint8_t *)s;
    uint8_t *tmp = NULL;
    if (len <= UART_DMA_BOUNCE) {
        memcpy(u->dma_bounce, s, len);
        src = u->dma_bounce;
    } else {
        /* oversize: heap is in main SRAM, so a malloc'd temp is DMA-accessible.
         * uart_dma_write runs from thread context (never an ISR), so malloc is OK. */
        tmp = (uint8_t *)malloc(len);
        if (!tmp) return -1;
        memcpy(tmp, s, len);
        src = tmp;
    }
    u->dma_dev->fun->config(u->dma_dev, u->dma_tx, dr, (void *)src, (uint32_t)len,
                            DMA_DATA_8, 0 /*periph_inc*/, 1 /*mem_inc*/, DMA_PRIO_MED);
    uart_hal_enable_tx_dma(u->hal);
    u->dma_dev->fun->start(u->dma_dev, u->dma_tx, NULL, NULL);
    u->dma_dev->fun->wait_done(u->dma_dev, u->dma_tx, 0);
    uart_hal_disable_tx_dma(u->hal);
    if (tmp) free(tmp);
    return (int)len;
}

/* DMA receive: program the reserved RX stream (P2M, PAR=DR, memory=bounce, MINC),
 * gate the USART onto the DMA (DMAR), arm it and block until Transfer-Complete
 * (i.e. until `len` bytes have arrived). The DMA writes into the main-SRAM
 * bounce (DMA cannot touch CCM), then we copy out to the caller's buffer. */
static int uart_dma_read(uart *u, void *buf, size_t len)
{
    if (!u->dma_dev || !u->dma_rx) return -1;
    void *dr = uart_hal_get_dr_addr(u->hal);
    uint8_t *tmp = (len <= UART_DMA_BOUNCE) ? u->dma_bounce : (uint8_t *)malloc(len);
    if (!tmp) return -1;
    u->dma_dev->fun->config(u->dma_dev, u->dma_rx, dr, tmp, (uint32_t)len,
                            DMA_DATA_8, 0 /*periph_inc*/, 1 /*mem_inc*/, DMA_PRIO_MED);
    uart_hal_clear_errors(u->hal);   /* un-freeze receiver if an Overrun latched */
    uart_hal_enable_rx_dma(u->hal);
    u->dma_dev->fun->start(u->dma_dev, u->dma_rx, NULL, NULL);
    int rc = u->dma_dev->fun->wait_done(u->dma_dev, u->dma_rx, 2000);
    uart_hal_disable_rx_dma(u->hal);
    if (rc == 0) memcpy(buf, tmp, len);
    if (tmp != u->dma_bounce) free(tmp);
    return rc == 0 ? (int)len : -1;
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
            log_printf(app_log(), LOG_DEBUG, "uart", "[uart] %s: unknown TX signal \"%s\"\n", u->parent.parent.name, u->tx_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "uart", "[uart] %s: TX pin P%c%d CONFLICT — refused\n",
                   u->parent.parent.name, 'A' + port, pin);
            return -2;                       /* conflict: do NOT configure */
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);

        /* RX */
        if (!pinmux_hal_resolve(u->rx_signal, &port, &pin, &af)) {
            log_printf(app_log(), LOG_DEBUG, "uart", "[uart] %s: unknown RX signal \"%s\"\n", u->parent.parent.name, u->rx_signal);
            pm->fun->release_owner(pm, u->parent.parent.name);   /* roll back TX */
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "uart", "[uart] %s: RX pin P%c%d CONFLICT — refused\n",
                   u->parent.parent.name, 'A' + port, pin);
            pm->fun->release_owner(pm, u->parent.parent.name);   /* roll back TX */
            return -2;
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);
    }

    uart_hal_init(u->hal);

    /* Reserve the hard-wired DMA streams for this UART's TX/RX (if any). This is
     * idempotent across open/close — close() releases them. */
    uart_dma_acquire(u);

    /* Attach our RX storage to the embedded ring buffer (common/ringbuffer) so
     * the receive ISR can push bytes and read()/getc() can drain them. */
    stream_device_init_ringbuffer((stream_device *)u, (uint8_t *)u->rx_buf, UART_RX_BUF_SIZE);

    /* Wire the RX interrupt through the PLATFORM-INDEPENDENT irq framework.
     * The driver registers a callback + its own context; the HAL supplies the
     * chip IRQ number (uart_hal_irq_id), so the driver never names a Cortex-M /
     * STM32 interrupt directly. The ISR (uart_isr) reads DR and fills the ring
     * buffer; read()/getc() then drain it. */
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_manager_set_priority(id, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    /* Choose the RX engine: DMA_IDLE arms a circular RX DMA + IDLE interrupt
     * (zero per-byte ISR); IRQ enables the per-byte RXNE ISR; POLL/DMA(bulk)
     * leave the data IRQs off (the loop or DMA-complete drives reception). */
    if (u->parent.mode == STREAM_MODE_DMA_IDLE) {
        if (u->dma_rx) uart_idle_dma_arm(u);
        else uart_hal_enable_rx_irq(u->hal);   /* no DMA route: fall back to IRQ */
    } else if (u->parent.mode == STREAM_MODE_IRQ) {
        uart_hal_enable_rx_irq(u->hal);
    }
    irq_manager_attach(id, uart_isr, u);    /* register handler via the manager */
    irq_manager_enable(id, uart_isr, u);    /* arm NVIC (safe: callback present) */
    return 0;
}

static int uart_dev_close(device *self)
{
    uart *u = (uart *)self;
    uart_dma_release(u);               /* release reserved DMA streams */
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_manager_detach(id, uart_isr, u); /* mask NVIC + uninstall callback */
    uart_hal_disable_rx_irq(u->hal);
    uart_hal_disable_tx_irq(u->hal);
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
    if (u->parent.mode == STREAM_MODE_DMA) return uart_dma_read(u, buf, len);
    if (u->parent.mode == STREAM_MODE_POLL) {
        while (!uart_hal_rx_pending(u->hal)) { }        /* busy-wait, no ISR */
        *(char *)buf = uart_hal_read_dr(u->hal);
        return 1;
    }
    /* IRQ mode: the ISR feeds a ring buffer. Return NON-blocking so a caller can
     * poll without stalling its loop (the main command loop also services other
     * devices such as USB). If a byte is present, pop it immediately. */
    ringbuffer *rb = stream_device_get_ringbuffer(self);
    if (rb && rb->fun->is_empty(rb)) return 0;
    *(char *)buf = uart_rx_getc(u);      /* IRQ: ISR-fed ring — byte is ready */
    return 1;
}

static int uart_stream_write(stream_device *self, const void *buf, size_t len)
{
    uart *u = (uart *)self;
    const char *s = (const char *)buf;
    if (!buf) return -1;
    if (u->parent.mode == STREAM_MODE_DMA) return uart_dma_write(u, s, len);
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
        if (m != STREAM_MODE_POLL && m != STREAM_MODE_IRQ &&
            m != STREAM_MODE_DMA && m != STREAM_MODE_DMA_IDLE)
            return -1;
        /* Leaving DMA_IDLE: stop the idle-line receiver (IDLE + DMAR). */
        if (u->parent.mode == STREAM_MODE_DMA_IDLE)
            uart_idle_dma_disarm(u);
        if (m == STREAM_MODE_DMA) {
            /* only allowed if this UART reserved a DMA stream at open() */
            if (!u->dma_tx && !u->dma_rx) return -1;
            /* DMA moves the bytes itself; silence the UART's own data IRQs so the
             * ISR cannot steal a byte the DMA is supposed to transfer. */
            uart_hal_disable_tx_irq(u->hal);
            uart_hal_disable_rx_irq(u->hal);
        }
        u->parent.mode = m;
        /* (Re)select the RX engine for the new mode. */
        if (m == STREAM_MODE_DMA_IDLE) {
            if (u->dma_rx) uart_idle_dma_arm(u);
            else uart_hal_enable_rx_irq(u->hal);   /* no DMA route: fall back */
        } else if (m == STREAM_MODE_IRQ) {
            uart_hal_enable_rx_irq(u->hal);
        } else {
            uart_hal_disable_rx_irq(u->hal);
            uart_hal_disable_tx_irq(u->hal);
        }
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
