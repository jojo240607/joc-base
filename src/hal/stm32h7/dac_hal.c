#include "dac_hal.h"
#include "stm32h750xx.h"
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — DAC (STM32H7 DAC1, channels 1/2).
 *
 * The H7 DAC has the same register layout as the F4 DAC (CR/SWTRIGR/DHR/DOR/SR).
 * The only differences from F4 are:
 *   - Clock gate: RCC->APB1LENR.DAC12EN (not RCC->APB1ENR.DACEN)
 *   - DAC1 is in the D2 APB1 domain @ 0x40007400 (DAC1_BASE)
 *   - H7 has two DAC instances (DAC1, DAC2); this HAL covers DAC1 only.
 */

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
    { if (h) RCC->APB1LENR |= RCC_APB1LENR_DAC12EN; }

void dac_hal_enable_channel(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
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

uint16_t dac_hal_get_dhr(dac_hal_handle_t *h, int ch)
{
    if (!h) return 0;
    return (ch == 2) ? (uint16_t)(h->reg->DHR12R2 & 0x0FFFU)
                     : (uint16_t)(h->reg->DHR12R1 & 0x0FFFU);
}

uint32_t dac_hal_get_cr(dac_hal_handle_t *h) { return h ? h->reg->CR : 0U; }

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
    uint32_t sel = (tsel & 0x7UL);
    if (ch == 2) {
        h->reg->CR &= ~(DAC_CR_TEN2 | (0x7UL << 17));
        h->reg->CR |=  DAC_CR_TEN2 | (sel << 17);
    } else {
        h->reg->CR &= ~(DAC_CR_TEN1 | (0x7UL << 1));
        h->reg->CR |=  DAC_CR_TEN1 | (sel << 1);
    }
}

void dac_hal_disable_trigger(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR &= ~DAC_CR_TEN2;
    else         h->reg->CR &= ~DAC_CR_TEN1;
}