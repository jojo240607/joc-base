#include "exti_hal.h"
#include <stm32f103xx.h>    /* EXTI_TypeDef, AFIO_TypeDef, RCC, IRQn_Type */
#include <stdlib.h>

/*
 * STM32F1 EXTI HAL.
 *
 * Key difference from F4: F1 uses AFIO (Alternate Function I/O) instead of
 * SYSCFG for routing GPIO ports to EXTI lines. AFIO->EXTICR[0..3] replaces
 * SYSCFG->EXTICR[0..3].
 */

/* OPAQUE handle — only the (port, pin) that defines the EXTI line. */
struct exti_hal_handle {
    pinmux_port_t port;   /* port routed to the EXTI line via AFIO EXTICR */
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

void exti_hal_select_source(exti_hal_handle_t *h)
{
    if (!h) return;
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;   /* AFIO clock (EXTICR lives there) */
    uint32_t idx   = (uint32_t)(h->pin >> 2);          /* EXTICR[pin/4] */
    uint32_t shift = ((uint32_t)(h->pin & 3U)) * 4U;   /* field within the reg */
    uint32_t val = (uint32_t)h->port << shift;
    uint32_t mask = ~(0xFU << shift);
    switch (idx) {
        case 0: AFIO->EXTICR1 = (AFIO->EXTICR1 & mask) | val; break;
        case 1: AFIO->EXTICR2 = (AFIO->EXTICR2 & mask) | val; break;
        case 2: AFIO->EXTICR3 = (AFIO->EXTICR3 & mask) | val; break;
        case 3: AFIO->EXTICR4 = (AFIO->EXTICR4 & mask) | val; break;
    }
}

void exti_hal_set_edge(exti_hal_handle_t *h, int edge_mask)
{
    if (!h) return;
    uint32_t b = exti_bit(h);
    if (edge_mask & EXTI_EDGE_RISING)  EXTI->RTSR |=  b;
    else                               EXTI->RTSR &= ~b;
    if (edge_mask & EXTI_EDGE_FALLING) EXTI->FTSR |=  b;
    else                               EXTI->FTSR &= ~b;
}

void exti_hal_unmask(exti_hal_handle_t *h) { if (h) EXTI->IMR |=  exti_bit(h); }
void exti_hal_mask(exti_hal_handle_t *h)   { if (h) EXTI->IMR &= ~exti_bit(h); }

int exti_hal_pending(exti_hal_handle_t *h)
{
    return (h && (EXTI->PR & exti_bit(h))) ? 1 : 0;
}

/* PR is cleared by writing 1; doing so also clears a pending SWIER request. */
void exti_hal_clear(exti_hal_handle_t *h) { if (h) EXTI->PR = exti_bit(h); }

void exti_hal_software_trigger(exti_hal_handle_t *h)
{
    if (h) EXTI->SWIER |= exti_bit(h);
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