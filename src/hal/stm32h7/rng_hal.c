#include "rng_hal.h"
#include "stm32h7xx.h"
#include <stdlib.h>

/* H7 RNG: D3 AHB2 clock gate (RCC_AHB2ENR.RNGEN, bit 6) — same bit position
 * as the F4, different bus. CR/SR/DR register layout is identical. NOTE: the
 * H7 also adds health-test registers (HTCFG/HTCR) after DR; unused here. */

struct rng_hal_handle {
    RNG_TypeDef *reg;
};

rng_hal_handle_t *rng_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    rng_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (RNG_TypeDef *)peripheral;
    return h;
}
void rng_hal_destroy(rng_hal_handle_t *h) { free(h); }

void rng_hal_enable(rng_hal_handle_t *h)
{
    (void)h;
    /* Gate the RNG on the D3 AHB2 bus, then start the generator. The RNG uses
     * no external clock — it derives entropy from analog noise internally. */
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    RNG->CR |= RNG_CR_RNGEN;
}

uint32_t rng_hal_get_u32(rng_hal_handle_t *h)
{
    if (!h) return 0U;
    /* DRDY is set by hardware when a new 32-bit word is ready; it clears on
     * read of RNG_DR. We never enable the RNG interrupt, so no ISR owns this
     * flag and a simple busy-wait is correct (no IRQ/deadlock hazard). */
    while ((h->reg->SR & RNG_SR_DRDY) == 0U) { }
    return h->reg->DR;
}

uint32_t rng_hal_get_status(rng_hal_handle_t *h)
{
    return h ? h->reg->SR : 0U;
}

int rng_hal_is_error(rng_hal_handle_t *h)
{
    if (!h) return 1;
    return (h->reg->SR & (RNG_SR_CEIS | RNG_SR_SEIS)) != 0U;
}
