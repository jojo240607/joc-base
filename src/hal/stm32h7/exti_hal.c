#include "exti_hal.h"
#include <stm32h7xx.h>    /* EXTI_TypeDef, IRQn_Type */
#include <stdlib.h>

/*
 * STM32H7 EXTI HAL.
 *
 * Key difference from F4/F1: H7 has NO EXTICR register. GPIO lines are wired
 * DIRECTLY to the EXTI block (line N of EXTI <-> pin N of EVERY port), so
 * select_source is a no-op and the line is identified by the pin number alone.
 * Block 1 registers (RTSR1/FTSR1/SWIER1/IMR1/PR1) cover lines 0..31.
 */

/* OPAQUE handle — only the (port, pin) that defines the EXTI line. */
struct exti_hal_handle {
    pinmux_port_t port;   /* kept for diagnostics; no register effect on H7 */
    uint8_t pin;          /* pin number 0..15 (selects the EXTI line) */
};

exti_hal_handle_t *exti_hal_create(pinmux_port_t port, uint8_t pin)
{
    if (pin >= 16U) return NULL;
    exti_hal_handle_t *h = (exti_hal_handle_t *)malloc(sizeof(exti_hal_handle_t));
    if (!h) return NULL;
    h->port = port;
    h->pin  = pin;
    return h;
}

void exti_hal_destroy(exti_hal_handle_t *h)
{
    free(h);
}

static uint32_t exti_bit(const exti_hal_handle_t *h) { return 1U << h->pin; }

/* No EXTICR on H7: every GPIO port drives the same EXTI line per pin number. */
void exti_hal_select_source(exti_hal_handle_t *h)
{
    (void)h;
}

void exti_hal_set_edge(exti_hal_handle_t *h, int edge_mask)
{
    if (!h) return;
    uint32_t b = exti_bit(h);
    if (edge_mask & EXTI_EDGE_RISING)  EXTI->RTSR1 |=  b;
    else                               EXTI->RTSR1 &= ~b;
    if (edge_mask & EXTI_EDGE_FALLING) EXTI->FTSR1 |=  b;
    else                               EXTI->FTSR1 &= ~b;
}

void exti_hal_unmask(exti_hal_handle_t *h) { if (h) EXTI->IMR1 |=  exti_bit(h); }
void exti_hal_mask(exti_hal_handle_t *h)   { if (h) EXTI->IMR1 &= ~exti_bit(h); }

int exti_hal_pending(exti_hal_handle_t *h)
{
    return (h && (EXTI->PR1 & exti_bit(h))) ? 1 : 0;
}

/* PR1 is cleared by writing 1; doing so also clears a pending SWIER request. */
void exti_hal_clear(exti_hal_handle_t *h) { if (h) EXTI->PR1 = exti_bit(h); }

void exti_hal_software_trigger(exti_hal_handle_t *h)
{
    if (h) EXTI->SWIER1 |= exti_bit(h);
}

irq_id_t exti_hal_irq_id(exti_hal_handle_t *h)
{
    if (!h) return -1;
    switch (h->pin) {
        case 0:  return (irq_id_t)EXTI0_IRQn;
        case 1:  return (irq_id_t)EXTI1_IRQn;
        case 2:  return (irq_id_t)EXTI2_IRQn;
        case 3:  return (irq_id_t)EXTI3_IRQn;
        case 4:  return (irq_id_t)EXTI4_IRQn;
        default:
            if (h->pin <= 9U)  return (irq_id_t)EXTI9_5_IRQn;
            return (irq_id_t)EXTI15_10_IRQn;
    }
}
