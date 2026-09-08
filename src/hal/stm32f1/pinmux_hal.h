#ifndef PINMUX_HAL_H
#define PINMUX_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — pin multiplexing (STM32F1 implementation).
 *
 * STM32F1 GPIO register layout (CRL/CRH) is completely different from F4's
 * MODER/OTYPER/OSPEEDR/PUPDR. F1 also uses AFIO remap instead of per-pin
 * alternate-function numbers.
 *
 * The pinmux driver (drv/pinmux.c) is platform-independent and calls this
 * HAL for chip-specific configuration.
 */

typedef enum {
    PINMUX_PORT_A = 0,
    PINMUX_PORT_B,
    PINMUX_PORT_C,
    PINMUX_PORT_D,
    PINMUX_PORT_E,
    PINMUX_PORT_F,
    PINMUX_PORT_G,
    PINMUX_PORT_COUNT
} pinmux_port_t;

/* F1 electrical configuration for one pin.
 * On F1, the CRL/CRH register combines mode + CNF (unlike F4's separate
 * MODER/OTYPER/OSPEEDR/PUPDR).
 *   mode: 0=input, 1=output, 2=alternate function, 3=analog
 *   cnf:  0=push-pull(analog), 1=open-drain(floating input),
 *         2=alt-func push-pull(pull-up/down), 3=alt-func open-drain(reserved)
 *   af:   not used on F1 (no AFR registers); set to 0
 *   speed: 0=10MHz, 1=2MHz, 2=50MHz (maps to MODE bits 10MHz/2MHz/50MHz)
 *   pupd: 0=none, 1=pull-up, 2=pull-down (implemented via ODR when CNF=10 input) */
typedef struct {
    uint8_t af;     /* unused on F1 */
    uint8_t mode;   /* 0=input, 1=output, 2=alternate, 3=analog */
    uint8_t otype;  /* 0=push-pull, 1=open-drain (maps to CNF bit0) */
    uint8_t speed;  /* 0=10MHz, 1=2MHz, 2=50MHz */
    uint8_t pupd;   /* 0=none, 1=pull-up, 2=pull-down */
} pinmux_pin_cfg_t;

/* Resolve a logical signal name to (port, pin). af is always 0 on F1. */
int pinmux_hal_resolve(const char *signal,
                       pinmux_port_t *port, uint8_t *pin, uint8_t *af);

/* Reverse lookup */
const char *pinmux_hal_signal_at(pinmux_port_t port, uint8_t pin, uint8_t af);

/* Program the GPIO CRL/CRH registers for one pin */
void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg);

/* Return the GPIO_TypeDef* for a port */
void *pinmux_hal_port_base(pinmux_port_t port);

#endif /* PINMUX_HAL_H */