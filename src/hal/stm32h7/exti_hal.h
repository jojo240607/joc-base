#ifndef EXTI_HAL_H
#define EXTI_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */
#include "pinmux_hal.h"      /* pinmux_port_t (the port routed to the EXTI line) */

/*
 * Hardware Abstraction Layer — External Interrupt (EXTI, STM32H7 implementation).
 *
 * An EXTI line is driven by a GPIO pin. The driver (drv/exti.c) owns the pin
 * (claimed through the pinmux as an input) and uses THIS HAL for the
 * STM32-specific routing + trigger logic:
 *   - H7 GPIO lines are DIRECTLY wired to EXTI (NO EXTICR register — unlike
 *     F4's SYSCFG and F1's AFIO), so select_source is a no-op;
 *   - EXTI has TWO register blocks; block 1 (RTSR1/FTSR1/IMR1/PR1/SWIER1)
 *     covers lines 0..31, which is all a GPIO pin can drive;
 *   - the NVIC irq id depends on the PIN NUMBER (EXTI0..4 are dedicated; pins
 *     5..9 share IRQ23, 10..15 share IRQ40) — so the platform-independent
 *     irq framework's multi-handler dispatch is what lets several pins on the
 *     same shared line each have their own handler (each guards on its own PR
 *     bit, exactly like the timer drivers guard on UIF).
 *
 * All register knowledge lives here; the driver never sees EXTI_TypeDef.
 */
typedef struct exti_hal_handle exti_hal_handle_t;

typedef enum {
    EXTI_EDGE_RISING  = 1,
    EXTI_EDGE_FALLING = 2,
    EXTI_EDGE_BOTH    = 3,
} exti_edge_t;

/* port + pin identify the EXTI line (pin number 0..15 selects the line). */
exti_hal_handle_t *exti_hal_create(pinmux_port_t port, uint8_t pin);
void exti_hal_destroy(exti_hal_handle_t *h);

/* No-op on H7: GPIO lines are directly connected to EXTI (no EXTICR), so the
 * port is implied by the pin number itself. Kept for API compatibility. */
void exti_hal_select_source(exti_hal_handle_t *h);

/* Configure trigger edges (mask of EXTI_EDGE_*). */
void exti_hal_set_edge(exti_hal_handle_t *h, int edge_mask);

/* Unmask/mask this line in EXTI IMR1. */
void exti_hal_unmask(exti_hal_handle_t *h);
void exti_hal_mask(exti_hal_handle_t *h);

/* Returns non-zero if THIS pin's pending bit (PR1) is set. Handlers on a shared
 * NVIC line MUST check this before acting (sibling-guard). */
int exti_hal_pending(exti_hal_handle_t *h);

/* Clear THIS pin's pending bit (write 1 to PR1). */
void exti_hal_clear(exti_hal_handle_t *h);

/* Software-trigger THIS line (sets SWIER1) to exercise the ISR without a real
 * edge; the interrupt fires if the line is unmasked. Used by the self-test. */
void exti_hal_software_trigger(exti_hal_handle_t *h);

/* NVIC irq id for this pin's line (EXTI0..EXTI4 / EXTI9_5 / EXTI15_10). */
irq_id_t exti_hal_irq_id(exti_hal_handle_t *h);

#endif /* EXTI_HAL_H */
