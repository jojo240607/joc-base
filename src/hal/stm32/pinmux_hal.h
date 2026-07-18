#ifndef PINMUX_HAL_H
#define PINMUX_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — pin multiplexing (STM32 implementation).
 *
 * The pinmux driver (drv/pinmux.c) is platform-independent: it holds ONLY the
 * runtime ownership state (which owner owns which pin/function) and calls this
 * HAL for two silicon-specific things:
 *   - resolving a logical peripheral signal name (e.g. "USART1_TX") to its
 *     (port, pin, AF) triple, and
 *   - programming the GPIO registers for one pin.
 * All chip knowledge (the STM32F4 alternate-function matrix and the GPIO
 * register layout) lives HERE, so porting to another chip = rewrite this HAL
 * only. The driver never sees GPIO_TypeDef.
 */

/* GPIO port identifiers. They double as the index into the ownership matrix
 * and into the (contiguous) GPIO register blocks, so keep them 0-based & in
 * alphabetical order. */
typedef enum {
    PINMUX_PORT_A = 0,
    PINMUX_PORT_B,
    PINMUX_PORT_C,
    PINMUX_PORT_D,
    PINMUX_PORT_E,
    PINMUX_PORT_F,
    PINMUX_PORT_G,
    PINMUX_PORT_H,
    PINMUX_PORT_I,
    PINMUX_PORT_COUNT
} pinmux_port_t;

/* Electrical configuration programmed into the GPIO registers for one pin. */
typedef struct {
    uint8_t af;     /* alternate-function number 0..15 (used only when mode == 2) */
    uint8_t mode;   /* 0 = input, 1 = output, 2 = alternate function, 3 = analog */
    uint8_t otype;  /* 0 = push-pull, 1 = open-drain (output / AF only) */
    uint8_t speed;  /* 0 = low, 1 = medium, 2 = high, 3 = very-high */
    uint8_t pupd;   /* 0 = none, 1 = pull-up, 2 = pull-down */
} pinmux_pin_cfg_t;

/* A concrete, unambiguous pin reference: the exact (port, pin, af) triple.
 * Drivers claim pins with THIS (via pinmux_request), not by signal name, so a
 * peripheral that can appear on several pins (e.g. USART1_TX on PA9 OR PB6) is
 * always resolved to the specific pin the board chose — no "first match wins"
 * ambiguity. A plain GPIO pin simply uses af = 0. */
typedef struct {
    pinmux_port_t port;   /* e.g. PINMUX_PORT_A */
    uint8_t pin;          /* 0..15 */
    uint8_t af;           /* 0 = GPIO/analog, 1..15 = alternate function */
} pinmux_pin_t;

/* Resolve a logical signal name (e.g. "USART1_TX", "SPI2_SCK") to its
 * (port, pin, af). Returns 1 if found, 0 otherwise. The names are defined by
 * the AF database in pinmux_af_stm32f4.c. */
int pinmux_hal_resolve(const char *signal,
                       pinmux_port_t *port, uint8_t *pin, uint8_t *af);

/* Reverse lookup: return the first signal name mapped to (port, pin, af), or
 * NULL. Used only for human-readable diagnostics (pinmux_dump). */
const char *pinmux_hal_signal_at(pinmux_port_t port, uint8_t pin, uint8_t af);

/* Program the GPIO registers for one pin from cfg (enables the port clock). */
void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg);

/* Return the GPIO_TypeDef* for a port (for diagnostics); NULL if invalid. */
void *pinmux_hal_port_base(pinmux_port_t port);

#endif /* PINMUX_HAL_H */
