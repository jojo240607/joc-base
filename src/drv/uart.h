#ifndef UART_H
#define UART_H

#include "iface/device.h"
#include "uart_hal.h"         /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"       /* pinmux_pin_t (explicit pin tuple for the board) */
#include <stdint.h>

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
    device parent;                /* unified interface — MUST be first member */
    const struct uartFun *fun;
    uart_hal_handle_t *hal;       /* opaque — driver never dereferences it */
    pinmux_pin_t tx;              /* cached TX pin (port, pin, af) */
    pinmux_pin_t rx;              /* cached RX pin (port, pin, af) */
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
    /* Exact pins to claim, supplied by the board. Using the concrete
     * (port, pin, af) triple (not a signal name) means a peripheral that can
     * sit on several pins (USART1_TX on PA9 OR PB6) is always resolved to the
     * specific pin the board chose — no ambiguity. af = 7 for USART1. */
    pinmux_pin_t tx;        /* e.g. {PINMUX_PORT_A, 9, 7} */
    pinmux_pin_t rx;        /* e.g. {PINMUX_PORT_A, 10, 7} */
} uart_config_t;

/* console helpers (module-level singleton used by syscalls _write) */
void uart_set_console(uart *self);
void uart_console_putc(char c);

extern const struct uartFun uart_fun;

#endif /* UART_H */
