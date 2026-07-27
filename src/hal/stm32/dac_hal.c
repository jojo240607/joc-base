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

uint16_t dac_hal_get_dhr(dac_hal_handle_t *h, int ch)
{
    if (!h) return 0;
    return (ch == 2) ? (uint16_t)(h->reg->DHR12R2 & 0x0FFFU)
                     : (uint16_t)(h->reg->DHR12R1 & 0x0FFFU);
}

uint32_t dac_hal_get_cr(dac_hal_handle_t *h) { return h ? h->reg->CR : 0U; }

/* Return the channel's 12-bit right-aligned data-register address for the DMA
 * PAR. DHR12Rx is a 32-bit register; a 16-bit DMA transfer writes the lower
 * 12 bits correctly. */
void *dac_hal_get_dhr_addr(dac_hal_handle_t *h, int ch)
{
    if (!h) return NULL;
    return (ch == 2) ? (void *)&h->reg->DHR12R2 : (void *)&h->reg->DHR12R1;
}

/* Enable the DAC's DMA request for the channel. With TENx=0 (set by
 * dac_hal_enable_channel, static/no-trigger mode) each value the DMA writes
 * into DHR is auto-transferred to DOR, leaving DHR empty and raising the next
 * DMA request — so a single normal-mode DMA burst streams N samples. */
void dac_hal_enable_dma(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR |= DAC_CR_DMAEN2;
    else         h->reg->CR |= DAC_CR_DMAEN1;
}

/* Stop the DAC DMA request. */
void dac_hal_disable_dma(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR &= ~DAC_CR_DMAEN2;
    else         h->reg->CR &= ~DAC_CR_DMAEN1;
}

/* Enable trigger mode for a channel: TENx=1 and select the trigger source
 * (TSEL). With a timer TRGO as the trigger, each overflow moves DHR->DOR and
 * raises the DAC's DMA request, clocking a DMA burst. TENx is modified while
 * ENx stays set (set by dac_hal_enable_channel). TSEL1 is CR[3:1], TSEL2 is
 * CR[19:17] (3-bit field each). */
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

/* Back to static mode (TENx=0): DHR auto-transfers to DOR on write, no trigger
 * needed — so plain poll-mode writes work again. */
void dac_hal_disable_trigger(dac_hal_handle_t *h, int ch)
{
    if (!h) return;
    if (ch == 2) h->reg->CR &= ~DAC_CR_TEN2;
    else         h->reg->CR &= ~DAC_CR_TEN1;
}
