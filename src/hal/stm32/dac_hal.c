#include "dac_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

struct dac_hal_handle {
    DAC_TypeDef *reg;
};

dac_hal_handle_t *dac_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    dac_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (DAC_TypeDef *)peripheral;
    return h;
}
void dac_hal_destroy(dac_hal_handle_t *h) { free(h); }

void dac_hal_enable_clock(dac_hal_handle_t *h)
    { if (h) RCC->APB1ENR |= RCC_APB1ENR_DACEN; }

void dac_hal_enable_channel(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    /* ENx=1, TENx=0 (no trigger => DHR auto-transfers to DOR), WAVE=00, BOFF=0. */
    if (ch == 2) h->reg->CR |= DAC_CR_EN2;
    else          h->reg->CR |= DAC_CR_EN1;
}

void dac_hal_write(dac_hal_handle_t *h, int ch, uint16_t val)
{
    if (!h) return;
    if (ch == 2) h->reg->DHR12R2 = val & 0x0FFFU;
    else          h->reg->DHR12R1 = val & 0x0FFFU;
}

uint16_t dac_hal_get_dor(dac_hal_handle_t *h, int ch)
{
    if (!h) return 0;
    return (ch == 2) ? (uint16_t)(h->reg->DOR2 & 0x0FFFU)
                     : (uint16_t)(h->reg->DOR1 & 0x0FFFU);
}

uint32_t dac_hal_get_cr(dac_hal_handle_t *h) { return h ? h->reg->CR : 0U; }
