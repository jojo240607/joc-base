#ifndef UART_H
#define UART_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* uart IS-A stream_device (data stream) */
#include "uart_hal.h"         /* opaque handle ONLY — no STM32 types reach the driver */
#include "drv/dma.h"          /* dma / dma_stream_t (DMA engine) + pulls dma_hal.h (dma_req_id_t) */
#include "irq.h"              /* platform-independent interrupt API (irq_register/enable) */
#include "osal/osal.h"        /* osal_sem_t (TX completion + line serialization) */
#include <stdint.h>

/* Size of the RX ring buffer fed by the UART receive ISR. */
#define UART_RX_BUF_SIZE 64

/* DMA-accessible (main SRAM) bounce scratch used by the DMA TX/RX paths. The
 * STM32F4 DMA controllers CANNOT reach CCM (0x10000000) — only the CPU can — so
 * any caller buffer living on the CCM stack (or a CCM heap) is invalid as a DMA
 * source/destination and would raise a Transfer-Error instead of completing.
 * The uart struct itself is malloc'd in main SRAM, so this member is safe; we
 * DMA through it and copy to/from the caller's (possibly CCM) buffer. */
#define UART_DMA_BOUNCE 256

/* device-level control commands for the UART driver */
#define UART_IOCTL_SET_BAUDRATE 0x01   /* arg: const uint32_t* baud */
#define UART_IOCTL_GET_BAUDRATE 0x02   /* arg: uint32_t* baud */
#define UART_IOCTL_GET_BRR      0x03   /* arg: uint32_t* BRR register */
#define UART_IOCTL_GET_CR1      0x04   /* arg: uint32_t* CR1 register */
/* framing axis (USART-specific, orthogonal to the engine in stream_device.h):
 * NONE = no frame-end detection; IDLE = the USART IDLE interrupt marks the end
 * of a variable-length RX frame. IDLE is valid with BOTH the IRQ and DMA
 * engines (they share the same NDTR/ring flush logic), so it is NOT a 4th
 * engine and never appears as a STREAM_MODE_* value. */
#define UART_IOCTL_SET_FRAMING  0x05   /* arg: const uart_frame_t* */
#define UART_IOCTL_GET_FRAMING  0x06   /* arg: uart_frame_t* */

/* framing (RX frame-end detection) axis — see the comment above. */
typedef enum {
    UART_FRAME_NONE = 0,   /* no frame-end detection (raw byte stream) */
    UART_FRAME_IDLE,       /* USART IDLE line marks end of a variable-length frame */
} uart_frame_t;

/*
 * Driver layer — generic UART. Platform-independent: holds ONLY an opaque
 * `uart_hal_handle_t *`. Switching chips = rewrite hal/<new-platform>/uart_hal
 * only. Implements the unified `device` interface.
 */
typedef struct _uart uart;

struct uartFun {
    void (*destroy)(uart *self);
    void (*init)(uart *self);
    void (*deinit)(uart *self);
    void (*set_baudrate)(uart *self, uint32_t baud);
    char (*getc)(uart *self);       /* blocking receive (typed method) */
};

/*
 * Per-engine state — heap-allocated ONCE in open() according to the chosen
 * (engine, framing) pair, and freed in close(). This replaces the old static
 * dma_bounce[256] + dma_idle_buf[256] + rx_buf[64] (≈576 B) that EVERY uart
 * paid up-front even when it never used DMA, so a POLL/IRQ UART now costs ~0 /
 * ~80 B instead of ~576 B. The control block (TX state machine + async RX
 * handoff) is the FIRST member of every variant, so it can be reached through a
 * single `uart_ctl_t *` cast regardless of which engine is active; POLL leaves
 * eng == NULL and never touches the control block.
 */
typedef struct {
    /* TX state machine, driven by the TXE ISR. Serializes ALL transmission on
     * one UART (blocking write, console printf, async submit WRITE) so they
     * never corrupt each other on the wire. */
    const char *tx_ptr;        /* next byte to send (thread sets, ISR advances) */
    size_t tx_rem;             /* bytes remaining to send */
    io_xfer_t *async_tx;       /* non-NULL => the in-progress TX is an async xfer */
    osal_sem_t tx_idle;        /* 1 = line free, 0 = a TX is in progress */
    osal_sem_t tx_done_sem;    /* signaled when a blocking TX finishes */
    /* In-progress asynchronous READ (started via stream submit). The ISR drains
     * the ring into this xfer and calls io_xfer_complete() when it is full.
     * NULL when no async read is pending. */
    io_xfer_t *async_rx;
} uart_ctl_t;

/* IRQ engine: control block + the per-byte RX ring storage (the embedded ring
 * buffer object is heap-allocated by stream_device_init_ringbuffer and backed
 * by this storage). */
typedef struct {
    uart_ctl_t ctl;
    char rx_storage[UART_RX_BUF_SIZE];
} uart_irq_t;

/* DMA engine: control block + a main-SRAM bounce (TX source / bulk RX dest).
 * The IDLE circular ring is an OPTIONAL TAIL — present only when framing ==
 * UART_FRAME_IDLE, so allocating without it (DMA + NONE) saves ~264 B. */
typedef struct {
    uart_ctl_t ctl;
    uint8_t dma_bounce[UART_DMA_BOUNCE];
    /* --- IDLE tail (allocated only when framing == UART_FRAME_IDLE) --- */
    uint8_t idle_buf[UART_DMA_BOUNCE];
    uint32_t idle_total;     /* total bytes DMA has written (NDTR-based, mod size) */
    uint32_t idle_bufsize;   /* circular buffer size (= UART_DMA_BOUNCE) */
} uart_dma_t;

struct _uart {
    stream_device parent;         /* unified interface — MUST be first member (IS-A stream_device).
                                     parent.mode holds the ENGINE (POLL/IRQ/DMA) */
    const struct uartFun *fun;
    uart_hal_handle_t *hal;       /* opaque — driver never dereferences it */
    const char *tx_signal;        /* cached TX signal name (resolved at open) */
    const char *rx_signal;        /* cached RX signal name (resolved at open) */
    /* framing (RX frame-end detection) axis — orthogonal to the engine:
     * NONE (raw byte stream) or IDLE (USART IDLE line marks frame end). */
    uart_frame_t framing;
    /* DMA engine handles (valid only when engine == STREAM_MODE_DMA and the
     * streams were successfully acquired at open). Kept as always-present small
     * pointers so TX/RX can arm a transfer without re-resolving the route. */
    dma_req_id_t dma_tx_req;      /* cached from config (for re-acquire on reopen) */
    dma_req_id_t dma_rx_req;
    dma *dma_dev;                 /* resolved dma controller (dma1/dma2) */
    dma_stream_t *dma_tx;         /* reserved TX stream handle (NULL if none) */
    dma_stream_t *dma_rx;         /* reserved RX stream handle (NULL if none) */
    int dma_tx_dir;               /* cached direction for config() */
    int dma_rx_dir;
    /* per-engine state (see uart_ctl_t / uart_irq_t / uart_dma_t above). NULL for
     * POLL (zero state); heap-allocated in open() for IRQ / DMA, freed in close(). */
    void *eng;
};

device *uart_create(const void *config);
void uart_destroy(uart *self);
void uart_init(uart *self);
void uart_deinit(uart *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; uart_create() reads it. */
typedef struct {
    const char *name;       /* logical device name */
    void *periph;           /* USART1 (board layer only) */
    uint32_t baud;          /* baud rate */
    uint8_t is_console;     /* 1 => install as the printf console */
    /* Signal names to claim, supplied by the board. The pinmux resolves each
     * name (e.g. "USART1_TX_PA9") to its exact (port, pin, af) — so a peripheral
     * that can sit on several pads is chosen unambiguously by name, with no
     * duplicate-name ambiguity in the AF database (suffixes guarantee uniqueness). */
    const char *tx_signal;  /* e.g. "USART1_TX_PA9" */
    const char *rx_signal;  /* e.g. "USART1_RX_PA10" */
    /* DMA request IDs (logical, from dma_hal.h). The driver resolves each to a
     * concrete (controller, stream, channel) via dma_hal_route() and acquires
     * that stream from the matching dma device — there is no other way to know
     * which DMA stream a USART TX/RX is hard-wired to. 0 (DMA_REQ_NONE) means
     * "no DMA for this direction" (the driver then refuses STREAM_MODE_DMA). */
    dma_req_id_t dma_tx_req;
    dma_req_id_t dma_rx_req;
    /* Default RX engine (stream_device.h POLL/IRQ/DMA) and framing
     * (UART_FRAME_NONE / UART_FRAME_IDLE) chosen at open(). The board picks
     * these; framing == IDLE is only valid together with IRQ or DMA. */
    stream_xfer_mode_t engine;
    uart_frame_t framing;
} uart_config_t;

/* console helpers (module-level singleton used by syscalls _write) */
void uart_set_console(uart *self);
void uart_console_putc(char c);

/* 二进制安全的控制台字节发送（不做 \n->\r 转换）：用于 gcov 覆盖率的 .gcda
 * 帧透传（docs/rtos-test-plan.md §6.6），避免文本模式对 0x0A 插入 CR 破坏数据。 */
void uart_console_raw(const uint8_t *p, size_t n);

/* 诊断：console 的 HAL 句柄（void* 不透明），供内核断言在临界区/ISR 内做
 * 纯 polling 串口打印（见 rtos_sched_assert_fail）。无 console 时为 NULL。 */
extern void *g_debug_uart_hal;

extern const struct uartFun uart_fun;

#endif /* UART_H */
