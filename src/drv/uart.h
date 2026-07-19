#ifndef UART_H
#define UART_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* uart IS-A stream_device (data stream) */
#include "uart_hal.h"         /* opaque handle ONLY — no STM32 types reach the driver */
#include "irq.h"              /* platform-independent interrupt API (irq_register/enable) */
#include "osal/osal.h"        /* osal_sem_t (TX completion + line serialization) */
#include <stdint.h>

/* Size of the RX ring buffer fed by the UART receive ISR. */
#define UART_RX_BUF_SIZE 64

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
    /* RX storage handed to the embedded ring buffer (stream_device.rx_rb, the
     * common/ringbuffer class). The receive ISR pushes bytes via that ring;
     * read()/getc() drain it. head/tail now live inside the ring buffer. */
    char rx_buf[UART_RX_BUF_SIZE];
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
} uart_config_t;

/* console helpers (module-level singleton used by syscalls _write) */
void uart_set_console(uart *self);
void uart_console_putc(char c);

extern const struct uartFun uart_fun;

#endif /* UART_H */
