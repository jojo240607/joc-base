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

/* 诊断钩子：暴露 console 的 HAL 句柄给内核断言诊断打印用（纯 polling 发送，
 * 不依赖中断，可在临界区/ISR 安全调用）。见 rtos_sched_assert_fail。 */
void *g_debug_uart_hal = (void *)0;

/* virtual implementations dispatched through the unified device vtable */
static int uart_dev_open(device *self);
static int uart_dev_close(device *self);
static int uart_dev_read(device *self, void *buf, size_t len);
static int uart_dev_write(device *self, const void *buf, size_t len);
static int uart_dev_ioctl(device *self, int cmd, void *arg);
static int uart_dev_irq_id(device *self);

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

/* per-engine state management (defined below). */
static uart_ctl_t *uart_ctl(uart *u);          /* control block (IRQ/DMA; NULL for POLL) */
static void uart_free_engine(uart *u);         /* disarm idle + free ring + eng (idempotent) */
static int uart_setup_engine(uart *u, stream_xfer_mode_t engine, uart_frame_t framing);
static void uart_select_rx_engine(uart *u);   /* enable/disable RX producer for (engine,framing) */
/* IDLE-line DMA RX helpers (defined further below, forward-declared for the ISR
 * and uart_select_rx_engine which call them before their definition). */
static void uart_idle_dma_arm(uart *u);
static void uart_idle_flush(uart *u);

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
    .irq_id = uart_dev_irq_id,
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
    self->parent.mode = c->engine;   /* RX engine from board config (POLL/IRQ/DMA) */
    self->framing     = c->framing;  /* framing axis (NONE/IDLE) from board config */
    if (c->is_console) uart_set_console(self);
    return (device *)self;
}

void uart_destroy(uart *self)
{
    if (!self) return;
    uart_free_engine(self);          /* disarm idle + free ring + eng (idempotent) */
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
    /* engine (parent.mode) and framing are set from the board config in
     * uart_create(); the per-engine state (semaphores, bounce, ring) is
     * heap-allocated in open() via uart_setup_engine(). */
    self->fun = &uart_fun;
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
    g_debug_uart_hal = self ? self->hal : (void *)0;
}

uart *uart_get_console(void)
{
    return g_console;
}

void uart_console_putc(char c)
{
    if (!g_console) return;
    /* Route through the TX state machine (IRQ engine, or DMA engine with IDLE
     * framing) so printf output is serialized with stream writes on the same
     * UART (no wire corruption). The bulk DMA (framing==NONE) and POLL paths
     * fall back to polling. The TX state machine lives in the per-engine control
     * block, so it is only present for IRQ / DMA engines (g_console->eng != NULL). */
    if (g_console->eng &&
        (g_console->parent.mode == STREAM_MODE_IRQ ||
         (g_console->parent.mode == STREAM_MODE_DMA &&
          g_console->framing == UART_FRAME_IDLE)))
        uart_tx_blocking(g_console, &c, 1);
    else
        uart_hal_putc(g_console->hal, c);
}

void uart_console_raw(const uint8_t *p, size_t n)
{
    if (!g_console || !p) return;
    for (size_t i = 0; i < n; i++) {
        /* 与 uart_console_putc 相同的 TX 路径，但【绝不】插入 CR，保证二进制干净。 */
        if (g_console->eng &&
            (g_console->parent.mode == STREAM_MODE_IRQ ||
             (g_console->parent.mode == STREAM_MODE_DMA &&
              g_console->framing == UART_FRAME_IDLE)))
            uart_tx_blocking(g_console, (const char *)&p[i], 1);
        else
            uart_hal_putc(g_console->hal, (char)p[i]);
    }
}

/* 纯轮询二进制透传：直接走 uart_hal_putc（忙等 TXE，不经过 TX 状态机 / RTOS 信号量），
 * 用于 gcov .gcda 这类大块二进制导出——避免 IRQ/DMA TX 状态机在高速连续发送下丢字节
 * 导致 .gcda 损坏（实测 DMA+IDLE 控制台下逐字节 uart_tx_blocking 会让 .gcda 在固定
 * 偏移处出现位翻转 / 丢字节）。默认 gcov g_out 指向它。 */
void uart_console_raw_poll(const uint8_t *p, size_t n)
{
    if (!g_console || !p) return;
    for (size_t i = 0; i < n; i++)
        uart_hal_putc(g_console->hal, (char)p[i]);
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

/* Pop one byte, blocking until the ISR delivers one (thread context).
 * Defensive: if the RX ring was never initialised (rb == NULL) do NOT
 * busy-wait forever — return 0 so the caller fails gracefully instead of
 * stalling the whole RTOS task. */
static char uart_rx_getc(uart *self)
{
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)self);
    uint8_t c = 0;
    if (!rb) return 0;
    while (rb->fun->is_empty(rb)) { }   /* wait for the ISR to fill */
    rb->fun->get(rb, &c);
    return (char)c;
}

/* ---------------------------------------------------------------------------
 * Per-engine state management
 *
 * The engine (POLL/IRQ/DMA) and the framing (NONE/IDLE) are two ORTHOGONAL axes.
 * At open() (and on every runtime STREAM_IOCTL_SET_MODE / UART_IOCTL_SET_FRAMING)
 * we (re)allocate exactly the state the chosen (engine, framing) needs and wire
 * up the RX producer:
 *   POLL            : eng == NULL (zero state)
 *   IRQ             : uart_irq_t   (control block + 64 B ring storage)
 *   DMA + NONE      : uart_dma_t   (control block + 256 B bounce, no IDLE ring)
 *   DMA + IDLE      : uart_dma_t   (control block + 256 B bounce + 256 B IDLE ring)
 * The control block (TX state machine + async-RX handoff) is the FIRST member of
 * every variant, so it is reached with a single cast regardless of engine.
 * ------------------------------------------------------------------------- */

/* Control block accessor — valid only for IRQ / DMA engines (eng != NULL). */
static uart_ctl_t *uart_ctl(uart *u)
    { return (uart_ctl_t *)u->eng; }

/* Disarm idle-line reception (disable IDLE + DMAR; pause the circular stream).
 * Keeps the stream from lingering in a CIRC/EN=1 state across a re-config. */
static void uart_idle_dma_disarm(uart *u)
{
    uart_hal_disable_idle_irq(u->hal);
    uart_hal_disable_rx_dma(u->hal);
    if (u->dma_dev && u->dma_rx)
        u->dma_dev->fun->stop(u->dma_dev, u->dma_rx);   /* EN=0, no TC/TE IRQ */
}

/* Tear down the current per-engine state. Idempotent: safe to call when eng is
 * already NULL. Disarms idle (if active), frees the RX ring, frees eng. */
static void uart_free_engine(uart *u)
{
    if (!u->eng) return;
    if (u->parent.mode == STREAM_MODE_DMA && u->framing == UART_FRAME_IDLE)
        uart_idle_dma_disarm(u);          /* stop the circular RX DMA first */
    stream_device_free_ringbuffer((stream_device *)u);  /* release RX ring (heap) */
    free(u->eng);
    u->eng = NULL;
}

/* (Re)build per-engine state for (engine, framing) and select the RX producer.
 * Validates BEFORE tearing down the current state, so an invalid request leaves
 * the uart untouched. Returns 0 on success, -1 on rejection. */
static int uart_setup_engine(uart *u, stream_xfer_mode_t engine, uart_frame_t framing)
{
    /* --- validate first (engines/framing combos) --- */
    if (engine != STREAM_MODE_POLL && engine != STREAM_MODE_IRQ &&
        engine != STREAM_MODE_DMA)
        return -1;
    if (framing != UART_FRAME_NONE && framing != UART_FRAME_IDLE)
        return -1;
    /* IDLE needs an interrupt/DMA producer; POLL has neither. */
    if (framing == UART_FRAME_IDLE && engine == STREAM_MODE_POLL)
        return -1;
    /* DMA engine requires a reserved DMA stream (acquired at open). */
    if (engine == STREAM_MODE_DMA && !u->dma_tx && !u->dma_rx)
        return -1;

    /* --- tear down current state --- */
    uart_free_engine(u);

    /* --- allocate + init the state the new (engine, framing) needs --- */
    if (engine == STREAM_MODE_IRQ) {
        uart_irq_t *e = (uart_irq_t *)malloc(sizeof(uart_irq_t));
        if (!e) {                       /* heap too tight: degrade to POLL */
            u->eng = NULL;
            u->parent.mode = STREAM_MODE_POLL;
            u->framing = (framing == UART_FRAME_IDLE) ? UART_FRAME_NONE : framing;
            uart_select_rx_engine(u);
            return 0;
        }
        memset(e, 0, sizeof(*e));
        osal_sem_init(&e->ctl.tx_idle, 1);   /* line starts free */
        u->eng = e;
        /* embedded ring storage (no extra heap alloc for the 64 B buffer) */
        stream_device_init_ringbuffer((stream_device *)u,
                                      (uint8_t *)e->rx_storage, UART_RX_BUF_SIZE);
    } else if (engine == STREAM_MODE_DMA) {
        /* omit the IDLE tail when framing == NONE to save ~264 B */
        size_t sz = (framing == UART_FRAME_IDLE)
                        ? sizeof(uart_dma_t)
                        : offsetof(uart_dma_t, idle_buf);
        uart_dma_t *e = (uart_dma_t *)malloc(sz);
        if (!e) {                       /* heap too tight: try IRQ, then POLL */
            if (uart_setup_engine(u, STREAM_MODE_IRQ, framing) == 0)
                return 0;
            u->eng = NULL;
            u->parent.mode = STREAM_MODE_POLL;
            u->framing = UART_FRAME_NONE;
            uart_select_rx_engine(u);
            return 0;
        }
        memset(e, 0, sz);
        osal_sem_init(&e->ctl.tx_idle, 1);
        u->eng = e;
        if (framing == UART_FRAME_IDLE) {
            e->idle_bufsize = UART_DMA_BOUNCE;
            e->idle_total   = 0;
            /* ring so IDLE-flushed bytes are readable via read()/getc() */
            stream_device_init_ringbuffer((stream_device *)u, NULL, UART_RX_BUF_SIZE);
        }
        /* DMA + NONE uses bulk per-read DMA and needs no standing RX ring. */
    } else { /* POLL: zero state */
        u->eng = NULL;
    }

    u->parent.mode  = engine;
    u->framing      = framing;
    uart_select_rx_engine(u);   /* enable/disable RXNE / IDLE / DMAR */
    return 0;
}

/* Enable/disable the RX producer according to (engine, framing). Called after
 * (re)allocating the per-engine state. Silences everything first, then arms the
 * one producer the active combination uses. */
static void uart_select_rx_engine(uart *u)
{
    stream_xfer_mode_t e  = u->parent.mode;
    uart_frame_t       fr = u->framing;

    uart_hal_disable_rx_irq(u->hal);    /* start silent */
    uart_hal_disable_idle_irq(u->hal);
    uart_hal_disable_rx_dma(u->hal);

    if (e == STREAM_MODE_POLL) {
        /* CPU polls DR; no IRQs, no DMA */
    } else if (e == STREAM_MODE_IRQ) {
        uart_hal_enable_rx_irq(u->hal);             /* RXNE -> ring */
        if (fr == UART_FRAME_IDLE)
            uart_hal_enable_idle_irq(u->hal);       /* IDLE marks frame end */
    } else { /* STREAM_MODE_DMA */
        if (fr == UART_FRAME_IDLE)
            uart_idle_dma_arm(u);                   /* circular RX DMA + IDLE */
        /* NONE: bulk uart_dma_read arms the one-shot RX DMA per call */
    }
}

/* --- IDLE-line DMA RX (engine=DMA, framing=IDLE) ---
 * A CIRCULAR RX DMA continuously drains DR into the IDLE ring; the USART IDLE
 * interrupt (bus idle >1 byte-time) marks the END of a variable-length frame.
 * On IDLE we compute how many bytes the DMA has written since the last flush
 * (via NDTR), copy that chunk from the circular buffer into the RX ring, and
 * re-arm. This gives a zero per-byte-ISR RX path while keeping read()/getc()
 * non-blocking and frame-length agnostic. (The same framing axis is also usable
 * with the IRQ engine — there RXNE already fills the ring and IDLE just marks
 * the frame boundary; the ISR clears IDLE but copies nothing extra.) */

/* Arm the circular RX DMA for idle-line reception. */
static void uart_idle_dma_arm(uart *u)
{
    if (!u->dma_dev || !u->dma_rx) return;
    uart_dma_t *e = (uart_dma_t *)u->eng;
    if (!e) return;
    void *dr = uart_hal_get_dr_addr(u->hal);
    e->idle_bufsize = UART_DMA_BOUNCE;
    /* P2M, PAR=DR, memory=idle_buf, MINC, 8-bit, circular (start_circular).
     * A SEPARATE buffer from dma_bounce so TX DMA never clobbers RX data. */
    u->dma_dev->fun->config(u->dma_dev, u->dma_rx, dr, e->idle_buf,
                            UART_DMA_BOUNCE, DMA_DATA_8, 0 /*periph_inc*/,
                            1 /*mem_inc*/, DMA_PRIO_MED);
    u->dma_dev->fun->start_circular(u->dma_dev, u->dma_rx);
    uart_hal_enable_rx_dma(u->hal);      /* DMAR: USART raises DMA requests */
    uart_hal_clear_idle(u->hal);         /* clear any stale IDLE (read SR then DR) */
    uart_hal_enable_idle_irq(u->hal);    /* IDLEIE: interrupt at frame end */
    e->idle_total = 0;                   /* NDTR-based running total */
}

/* Copy the bytes the circular RX DMA has written since the last flush into the
 * RX ring. Uses the running NDTR total so wrap-around is handled correctly. */
static void uart_idle_flush(uart *u)
{
    uart_dma_t *e = (uart_dma_t *)u->eng;
    if (!e) return;
    uint32_t n   = e->idle_bufsize;
    if (!n) return;
    uint32_t rem = u->dma_dev->fun->remaining(u->dma_dev, u->dma_rx); /* NDTR */
    uint32_t total = n - rem;             /* bytes written since arming (mod n) */
    int32_t d = (int32_t)total - (int32_t)e->idle_total;
    if (d < 0) d += (int32_t)n;           /* wrapped at least once */
    uint32_t newb = (uint32_t)d;
    uint32_t src  = e->idle_total % n;
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
    for (uint32_t i = 0; i < newb; i++)
        if (rb) rb->fun->put(rb, e->idle_buf[(src + i) % n]);
    e->idle_total = total;
}

/* Drain any bytes already buffered in the RX ring into an in-progress async
 * read, signalling completion when it is full. Shared by the IRQ and IDLE RX
 * paths (the producer is whichever engine is active). */
static void uart_rx_drain_async(uart *u)
{
    uart_ctl_t *c = uart_ctl(u);
    if (!c || !c->async_rx) return;
    io_xfer_t *x = c->async_rx;
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
    uint8_t cc;
    while (x->done < x->len && rb && rb->fun->get(rb, &cc) == 0)
        ((char *)x->buf)[x->done++] = (char)cc;
    if (x->done >= x->len) {        /* transfer complete */
        c->async_rx = NULL;
        io_xfer_complete(x, 0);      /* wake sync waiter + invoke callback */
    }
}

/* The receive ISR callback. Registered with the framework via irq_register()
 * (see uart_dev_open); `ctx` is the uart instance. In DMA+IDLE mode the IDLE
 * interrupt flushes the circular DMA chunk into the RX ring; in IRQ mode
 * each RXNE pushes a single byte. After either producer runs we drain the ring
 * into any in-progress async read. */
static void uart_isr(void *ctx)
{
    uart *u = (uart *)ctx;
    /* IDLE: a variable-length frame just ended (engine=DMA, framing=IDLE) —
     * flush the circular-DMA chunk to the RX ring. For IRQ+IDLE the ring is
     * already fed by RXNE, so we just clear the flag below. */
    if (u->parent.mode == STREAM_MODE_DMA &&
        u->framing == UART_FRAME_IDLE &&
        uart_hal_idle_pending(u->hal)) {
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
    uart_ctl_t *c = uart_ctl(u);
    if (!c) return;
    if (c->tx_rem > 0) {
        uart_hal_write_dr(u->hal, *c->tx_ptr++);
        c->tx_rem--;
        if (c->tx_rem == 0) {
            uart_hal_disable_tx_irq(u->hal);
            if (c->async_tx) {
                io_xfer_t *x = c->async_tx;
                c->async_tx = NULL;
                x->done = x->len;
                osal_sem_give(&c->tx_idle);   /* line free for the next TX */
                io_xfer_complete(x, 0);        /* wake async waiter + callback */
            } else {
                osal_sem_give(&c->tx_idle);
                osal_sem_give(&c->tx_done_sem);/* wake the blocking writer */
            }
        }
    } else {
        uart_hal_disable_tx_irq(u->hal);       /* nothing pending; silence */
    }
}

/* Blocking transmit through the TX state machine. Serialized with every other
 * TX (printf, async submit) via tx_idle, so they can never interleave on the
 * wire. Used by uart_stream_write (IRQ engine) and uart_console_putc. */
static int uart_tx_blocking(uart *u, const char *s, size_t len)
{
    uart_ctl_t *c = uart_ctl(u);
    if (!c || len == 0) return 0;
    osal_sem_wait(&c->tx_idle);          /* wait for the line to be free */
    c->async_tx = NULL;
    c->tx_ptr   = s;
    c->tx_rem   = len;
    osal_sem_init(&c->tx_done_sem, 0);
    uart_hal_enable_tx_irq(u->hal);      /* TXE ISR drains tx_rem */
    osal_sem_wait(&c->tx_done_sem);      /* block until the last byte is sent */
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
    uart_dma_t *e = (uart_dma_t *)u->eng;
    if (!e) return -1;
    void *dr = uart_hal_get_dr_addr(u->hal);
    const uint8_t *src = (const uint8_t *)s;
    uint8_t *tmp = NULL;
    if (len <= UART_DMA_BOUNCE) {
        memcpy(e->dma_bounce, s, len);
        src = e->dma_bounce;
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
    uart_dma_t *e = (uart_dma_t *)u->eng;
    if (!e) return -1;
    void *dr = uart_hal_get_dr_addr(u->hal);
    uint8_t *tmp = (len <= UART_DMA_BOUNCE) ? e->dma_bounce : (uint8_t *)malloc(len);
    if (!tmp) return -1;
    u->dma_dev->fun->config(u->dma_dev, u->dma_rx, dr, tmp, (uint32_t)len,
                            DMA_DATA_8, 0 /*periph_inc*/, 1 /*mem_inc*/, DMA_PRIO_MED);
    uart_hal_clear_errors(u->hal);   /* un-freeze receiver if an Overrun latched */
    uart_hal_enable_rx_dma(u->hal);
    u->dma_dev->fun->start(u->dma_dev, u->dma_rx, NULL, NULL);
    int rc = u->dma_dev->fun->wait_done(u->dma_dev, u->dma_rx, 2000);
    uart_hal_disable_rx_dma(u->hal);
    if (rc == 0) memcpy(buf, tmp, len);
    if (tmp != e->dma_bounce) free(tmp);
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

    /* (Re)build the per-engine state for the chosen (engine, framing) and wire
     * the RX producer (RXNE / IDLE / circular-DMA). POLL needs no state. */
    if (uart_setup_engine(u, u->parent.mode, u->framing) != 0) {
        /* requested engine unavailable (e.g. DMA requested but no DMA route
         * resolved) → fall back to interrupt-driven RX so the UART stays usable */
        uart_setup_engine(u, STREAM_MODE_IRQ, UART_FRAME_NONE);
    }

    /* Wire the RX interrupt through the PLATFORM-INDEPENDENT irq framework.
     * The driver registers a callback + its own context; the HAL supplies the
     * chip IRQ number (uart_hal_irq_id), so the driver never names a Cortex-M /
     * STM32 interrupt directly. The ISR (uart_isr) reads DR and fills the ring
     * buffer; read()/getc() then drain it. */
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_manager_set_priority(id, IRQ_PRIO_KERNEL, IRQ_CLASS_KERNEL);
    irq_manager_attach(id, uart_isr, u);    /* register handler via the manager */
    irq_manager_enable(id, uart_isr, u);    /* arm NVIC (safe: callback present) */
    return 0;
}

static int uart_dev_close(device *self)
{
    uart *u = (uart *)self;
    irq_id_t id = uart_hal_irq_id(u->hal);
    irq_manager_detach(id, uart_isr, u); /* mask NVIC + uninstall callback */
    uart_hal_disable_rx_irq(u->hal);
    uart_hal_disable_tx_irq(u->hal);
    uart_free_engine(u);               /* disarm idle DMA (needs dma stream) + free ring + eng */
    uart_dma_release(u);               /* release reserved DMA streams (after disarm) */
    uart_hal_deinit(u->hal);
    return 0;
}

/* stream-class ops — the REAL implementations; the base deviceVtable forwards
 * here so there is a single source of truth for the data path. The transfer
 * engine is chosen by self->parent.mode (POLL/IRQ/DMA, see stream_device.h);
 * framing only affects how RX detects frame boundaries (IDLE). */
static int uart_stream_read(stream_device *self, void *buf, size_t len)
{
    uart *u = (uart *)self;
    if (len < 1 || !buf) return -1;
    if (u->parent.mode == STREAM_MODE_POLL) {
        /* NON-blocking: the console command loop polls every device and sleeps
         * between iterations. A busy-wait here would stall the whole RTOS task
         * (and every lower-priority task) whenever no char is pending. */
        if (!uart_hal_rx_pending(u->hal)) return 0;
        *(char *)buf = uart_hal_read_dr(u->hal);
        return 1;
    }
    /* bulk one-shot DMA RX only when engine==DMA AND framing==NONE; otherwise the
     * RX producer (RXNE ISR, or circular-DMA+IDLE flush) feeds the ring. */
    if (u->parent.mode == STREAM_MODE_DMA && u->framing == UART_FRAME_NONE)
        return uart_dma_read(u, buf, len);
    /* IRQ, or DMA+IDLE: the ISR feeds a ring buffer. Return NON-blocking so a
     * caller can poll without stalling its loop (the main command loop also
     * services other devices such as USB). If a byte is present, pop it now. */
    ringbuffer *rb = stream_device_get_ringbuffer(self);
    if (!rb || rb->fun->is_empty(rb)) return 0;   /* no ring or empty → non-blocking */
    /* Ring is known non-empty here (checked above). Pop directly instead of
     * calling the blocking uart_rx_getc busy-wait, which would otherwise stall
     * the calling task (and every lower-priority task) whenever the ring state
     * is transiently inconsistent. */
    uint8_t c = 0;
    rb->fun->get(rb, &c);
    *(char *)buf = (char)c;
    return 1;
}

static int uart_stream_write(stream_device *self, const void *buf, size_t len)
{
    uart *u = (uart *)self;
    const char *s = (const char *)buf;
    if (!buf) return -1;
    if (u->parent.mode == STREAM_MODE_DMA)
        return uart_dma_write(u, s, len);             /* DMA engine TX */
    if (u->parent.mode == STREAM_MODE_IRQ)
        return uart_tx_blocking(u, s, len);           /* interrupt-driven TX */
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
    /* Bulk one-shot DMA (engine=DMA && framing=NONE) has no standing RX ring or
     * TX state machine to drive an async transfer, so it cannot submit — this
     * mirrors the old STREAM_MODE_DMA (rejected). All other combos (POLL, IRQ,
     * and DMA+IDLE, whose RX is ring-fed by the IDLE flush) DO support submit. */
    if (u->parent.mode == STREAM_MODE_DMA && u->framing == UART_FRAME_NONE)
        return -1;

    if (xfer->dir == IO_XFER_DIR_WRITE) {
        if (u->parent.mode == STREAM_MODE_POLL) {
            const char *s = (const char *)xfer->buf;
            for (size_t i = 0; i < xfer->len; i++)
                uart_hal_putc(u->hal, s[i]);    /* polling TX */
            xfer->done = xfer->len;
            io_xfer_complete(xfer, 0);          /* signal + callback (inline) */
            return 0;
        }
        /* IRQ (and DMA+IDLE RX, but TX here uses the IRQ state machine): drive TX
         * from the TXE ISR (serialized via ctl->tx_idle). The ISR calls
         * io_xfer_complete() when the last byte leaves. */
        uart_ctl_t *c = uart_ctl(u);
        if (!c) return -1;
        osal_sem_wait(&c->tx_idle);
        c->async_tx = xfer;
        c->tx_ptr   = (const char *)xfer->buf;
        c->tx_rem   = xfer->len;
        xfer->done  = 0;
        if (xfer->len == 0) {
            c->async_tx = NULL;
            osal_sem_give(&c->tx_idle);
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
    /* IRQ (and DMA+IDLE, whose IDLE flush fills the same ring): hand off to the
     * RX ISR. Drain any bytes already buffered, then let uart_isr() finish the
     * rest and call io_xfer_complete(). */
    uart_ctl_t *c = uart_ctl(u);
    if (!c) return -1;
    c->async_rx = xfer;
    ringbuffer *rb = stream_device_get_ringbuffer((stream_device *)u);
    uint8_t cc;
    while (xfer->done < xfer->len && rb && rb->fun->get(rb, &cc) == 0)
        ((char *)xfer->buf)[xfer->done++] = (char)cc;
    if (xfer->done >= xfer->len) {          /* all available already */
        c->async_rx = NULL;
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
    case UART_IOCTL_SET_INVERTED: {
        /* SBUS / inverted-peripheral logic-level inversion. arg is a const
         * uint32_t* bitmask: bit0 = RXINV (receive inverted),
         * bit1 = TXINV (transmit inverted). uart_hal_set_inverted() sets
         * USART_CR1.RXINV/TXINV without disturbing TE/RE/UE. */
        if (!arg) return -1;
        const uint32_t *p = (const uint32_t *)arg;
        int rx_inv = (int)((*p) & 0x1U);
        int tx_inv = (int)((*p) & 0x2U);
        uart_hal_set_inverted(u->hal, rx_inv, tx_inv);
        return 0;
    }
    case STREAM_IOCTL_SET_MODE: {
        if (!arg) return -1;
        stream_xfer_mode_t m = *(const stream_xfer_mode_t *)arg;
        /* (re)build per-engine state for the new engine, keeping the current
         * framing. uart_setup_engine validates the combo and returns -1 (leaving
         * the uart untouched) if the engine is unavailable. */
        return uart_setup_engine(u, m, u->framing);
    }
    case STREAM_IOCTL_GET_MODE:
        if (!arg) return -1;
        *(stream_xfer_mode_t *)arg = u->parent.mode;
        return 0;
    case UART_IOCTL_SET_FRAMING: {
        if (!arg) return -1;
        uart_frame_t fr = *(const uart_frame_t *)arg;
        /* (re)build per-engine state for the new framing, keeping the current
         * engine. uart_setup_engine rejects IDLE with POLL, etc. */
        return uart_setup_engine(u, u->parent.mode, fr);
    }
    case UART_IOCTL_GET_FRAMING:
        if (!arg) return -1;
        *(uart_frame_t *)arg = u->framing;
        return 0;
    default:
        return -1;
    }
}

static int uart_dev_irq_id(device *self)
{
    uart *u = (uart *)self;
    return (int)uart_hal_irq_id(u->hal);
}
