#include "exti_hal.h"
#include "stm32f4xx.h"     /* EXTI_TypeDef, SYSCFG_TypeDef, RCC, IRQn_Type */
#include <stdlib.h>

/* OPAQUE handle — only the (port, pin) that defines the EXTI line. */
struct exti_hal_handle {
    pinmux_port_t port;   /* port routed to the EXTI line via SYSCFG EXTICR */
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
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;   /* SYSCFG clock (EXTICR lives there) */
    uint32_t idx   = (uint32_t)(h->pin >> 2);          /* EXTICR[pin/4] */
    uint32_t shift = ((uint32_t)(h->pin & 3U)) * 4U;   /* field within the reg */
    SYSCFG->EXTICR[idx] = (SYSCFG->EXTICR[idx] & ~(0xFU << shift))
                        | ((uint32_t)h->port << shift); /* port index = EXTICR val */
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
