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

struct _uart {
    stream_device parent;         /* unified interface — MUST be first member (IS-A stream_device) */
    const struct uartFun *fun;
    uart_hal_handle_t *hal;       /* opaque — driver never dereferences it */
    const char *tx_signal;        /* cached TX signal name (resolved at open) */
    const char *rx_signal;        /* cached RX signal name (resolved at open) */
    /* DMA engine state (valid only when streams were successfully acquired at
     * open and mode == STREAM_MODE_DMA). The driver keeps the resolved dma
     * device + the two reserved stream handles so TX/RX can arm a transfer
     * without re-resolving the route each call. */
    dma_req_id_t dma_tx_req;      /* cached from config (for re-acquire on reopen) */
    dma_req_id_t dma_rx_req;
    dma *dma_dev;                 /* resolved dma controller (dma1/dma2) */
    dma_stream_t *dma_tx;         /* reserved TX stream handle (NULL if none) */
    dma_stream_t *dma_rx;         /* reserved RX stream handle (NULL if none) */
    int dma_tx_dir;               /* cached direction for config() */
    int dma_rx_dir;
    /* RX storage handed to the embedded ring buffer (stream_device.rx_rb, the
     * common/ringbuffer class). The receive ISR pushes bytes via that ring;
     * read()/getc() drain it. head/tail now live inside the ring buffer. */
    char rx_buf[UART_RX_BUF_SIZE];
    /* DMA bounce scratch in main SRAM (see UART_DMA_BOUNCE). Used as the actual
     * DMA source for TX (caller buffer may be CCM) and destination for bulk RX
     * (the caller's receive buffer may be CCM). The driver copies to/from it. */
    uint8_t dma_bounce[UART_DMA_BOUNCE];
    /* Dedicated circular buffer for the IDLE-line RX DMA. Must be SEPARATE from
     * dma_bounce: the TX DMA (uart_dma_write) and the circular RX DMA both need a
     * main-SRAM scratch, and sharing one would let a TX reuse clobber received
     * bytes (and vice-versa). DMA cannot touch CCM, so both live in main SRAM. */
    uint8_t dma_idle_buf[UART_DMA_BOUNCE];
    /* IDLE-line RX state (STREAM_MODE_DMA_IDLE only). The circular RX DMA keeps
     * filling dma_idle_buf; `idle_total` is the running count of bytes the DMA
     * has written since arming (NDTR-based, modulo the buffer size). On each IDLE
     * interrupt the ISR computes (idle_total - prev) new bytes and copies them
     * into the RX ring. */
    uint32_t dma_idle_total;     /* total bytes DMA has written (mod buffer) */
    uint32_t dma_idle_bufsize;   /* circular buffer size (= UART_DMA_BOUNCE) */
    /* In-progress asynchronous READ (started via stream submit). The ISR drains
     * the ring into this xfer and calls io_xfer_complete() when it is full.
     * NULL when no async read is pending. (Synchronous read()/getc() and an
     * async read must not be used on the same uart at the same time.) */
    io_xfer_t *async_rx;
    /* TX state machine, driven by the TXE ISR. Used by the blocking write(),
     * the console printf path (uart_console_putc) and the async submit WRITE so
     * that ALL transmission on one UART is serialized and never corrupts itself.
     * A single in-progress transfer owns the line; tx_idle (1 = free) makes the
     * next writer wait until the current one finishes. */
    const char *tx_ptr;        /* next byte to send (thread sets, ISR advances) */
    size_t tx_rem;             /* bytes remaining to send */
    io_xfer_t *async_tx;       /* non-NULL => the in-progress TX is an async xfer */
    osal_sem_t tx_idle;        /* 1 = line free, 0 = a TX is in progress */
    osal_sem_t tx_done_sem;    /* signaled when a blocking TX finishes */
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
} uart_config_t;

/* console helpers (module-level singleton used by syscalls _write) */
void uart_set_console(uart *self);
void uart_console_putc(char c);

extern const struct uartFun uart_fun;

#endif /* UART_H */
