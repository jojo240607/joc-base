#include "dac_hal.h"
#include "stm32f103xx.h"
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
{
    if (h) RCC->APB1ENR |= RCC_APB1ENR_DACEN;
}

void dac_hal_enable_channel(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR |= DAC_CR_EN2;
    else         h->reg->CR |= DAC_CR_EN1;
}

void dac_hal_write(dac_hal_handle_t *h, int ch, uint16_t val)
{
    if (!h) return;
    uint32_t v = val & 0x0FFFU;
    if (ch == 2) h->reg->DHR12R2 = v;
    else         h->reg->DHR12R1 = v;
}

uint16_t dac_hal_get_dor(dac_hal_handle_t *h, int ch)
{
    if (!h) return 0;
    return (uint16_t)((ch == 2) ? (h->reg->DOR2 & 0x0FFFU)
                                : (h->reg->DOR1 & 0x0FFFU));
}

uint16_t dac_hal_get_dhr(dac_hal_handle_t *h, int ch)
{
    if (!h) return 0;
    return (uint16_t)((ch == 2) ? (h->reg->DHR12R2 & 0x0FFFU)
                                : (h->reg->DHR12R1 & 0x0FFFU));
}

uint32_t dac_hal_get_cr(dac_hal_handle_t *h)
{
    return h ? h->reg->CR : 0;
}

void *dac_hal_get_dhr_addr(dac_hal_handle_t *h, int ch)
{
    if (!h) return NULL;
    return (ch == 2) ? (void *)&h->reg->DHR12R2 : (void *)&h->reg->DHR12R1;
}

void dac_hal_enable_dma(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR |= DAC_CR_DMAEN2;
    else         h->reg->CR |= DAC_CR_DMAEN1;
}

void dac_hal_disable_dma(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR &= ~DAC_CR_DMAEN2;
    else         h->reg->CR &= ~DAC_CR_DMAEN1;
}

void dac_hal_enable_trigger(dac_hal_handle_t *h, int ch, uint32_t tsel)
{
    if (!h) return;
    uint32_t mask = DAC_CR_TEN1 | DAC_CR_TSEL1;
    uint32_t val  = DAC_CR_TEN1 | ((tsel & 7U) << 3);
    if (ch == 2) { mask = DAC_CR_TEN2 | DAC_CR_TSEL2; val = DAC_CR_TEN2 | ((tsel & 7U) << 19); }
    h->reg->CR = (h->reg->CR & ~mask) | val;
}

void dac_hal_disable_trigger(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR &= ~DAC_CR_TEN2;
    else         h->reg->CR &= ~DAC_CR_TEN1;
}