#ifndef PINMUX_HAL_H
#define PINMUX_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — pin multiplexing (ESP32-C3 implementation).
 *
 * The pinmux driver (drv/pinmux.c) is platform-independent: it holds ONLY the
 * runtime ownership state and calls this HAL for signal resolution and GPIO
 * register programming. All chip knowledge lives HERE.
 *
 * Phase-1 note: ESP32-C3 uses a GPIO matrix (signals route through the matrix
 * registers, not an STM32-style AF table), and under Renode there is no GPIO
 * model. The board does not register a pinmux device in Phase-1, and every
 * function below is a stub — pinmux_hal_resolve() returns 0 (not found) so
 * drivers that claim pins at open() simply skip the claim step. The types
 * must stay identical to the STM32 HAL because drv/pinmux.h includes this
 * header unconditionally.
 */

/* GPIO port identifiers. They double as the index into the ownership matrix.
 * ESP32-C3 has a single GPIO block; the enum is kept for contract parity. */
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

/* Resolve a logical signal name to its (port, pin, af).
 * Phase-1 stub: no signal database, always returns 0 (not found). */
int pinmux_hal_resolve(const char *signal,
                       pinmux_port_t *port, uint8_t *pin, uint8_t *af);

/* Reverse lookup: NULL (no database). */
const char *pinmux_hal_signal_at(pinmux_port_t port, uint8_t pin, uint8_t af);

/* Program the GPIO registers for one pin (no-op, no GPIO model under Renode). */
void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg);

/* Return NULL (no register block exposed in Phase-1). */
void *pinmux_hal_port_base(pinmux_port_t port);

#endif /* PINMUX_HAL_H */
