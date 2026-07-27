#include "dma_hal.h"
#include "stm32f4xx.h"        /* DMA_TypeDef / DMA_Stream_TypeDef + CMSIS bit defs */
#include <stdlib.h>           /* malloc / free */
#include <string.h>

/*
 * STM32F4 DMA HAL — register-level stream operations, no driver/OOC knowledge.
 *
 * Register layout (per controller): LISR/HISR @ +0x00/+0x04, LIFCR/HIFCR
 * @ +0x08/+0x0C, then 8 streams at +0x10 stride +0x18 (CR/NDTR/PAR/M0AR/M1AR/
 * FCR). Flag bit positions inside a stream's 6-bit status group: FE=+0,
 * DME=+2, TE=+3, HT=+4, TC=+5; group starts at 6*(stream%4).
 */

struct dma_hal_stream {
    DMA_TypeDef  *dma;     /* DMA1 or DMA2 base */
    uint32_t      idx;     /* 0..7 */
    uint32_t      shift;   /* 6*(idx%4) — flag bit offset within L/H ISR/FCR */
    int           is_high; /* idx>=4 -> use HISR/HIFCR, else LISR/LIFCR */
    int           ctlr;    /* 1 = DMA1, 2 = DMA2 (for IRQn base) */
};

/* Compute the per-stream register block pointer from the controller base. */
static DMA_Stream_TypeDef *stream_reg(DMA_TypeDef *dma, uint32_t idx)
{
    return (DMA_Stream_TypeDef *)((uint8_t *)dma + 0x10 + 0x18 * idx);
}

dma_hal_stream_t *dma_hal_stream_create(void *dma_periph, uint32_t stream_idx)
{
    dma_hal_stream_t *s = (dma_hal_stream_t *)malloc(sizeof(dma_hal_stream_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->dma     = (DMA_TypeDef *)dma_periph;
    s->idx     = stream_idx & 0x7U;
    s->shift   = 6U * (s->idx % 4U);
    s->is_high = (s->idx >= 4U) ? 1 : 0;
    s->ctlr    = ((DMA_TypeDef *)dma_periph == DMA1) ? 1 : 2;
    return s;
}

void dma_hal_stream_destroy(dma_hal_stream_t *s)
{
    if (s) free(s);
}

void dma_hal_enable_clock(void *dma_periph)
{
    if ((DMA_TypeDef *)dma_periph == DMA1)
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    else
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
}

void dma_hal_stream_config(dma_hal_stream_t *s, dma_hal_dir_t dir, uint32_t channel,
                           const void *periph, void *mem, uint32_t count,
                           dma_hal_size_t size, int periph_inc, int mem_inc,
                           uint32_t prio)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);

    /* CR may only be written while EN=0: clear it and wait for the hardware to
     * acknowledge (a non-circular stream auto-clears EN on completion, but an
     * in-flight/circular one would not, so we must wait). */
    r->CR &= ~DMA_SxCR_EN;
    while (r->CR & DMA_SxCR_EN) { }

    /* Clear any stale interrupt flags for this stream before reprogramming. */
    dma_hal_stream_clear_flags(s);

    /* Program the data-path registers (NDTR is writable only when EN=0). */
    r->PAR  = (uint32_t)periph;
    r->M0AR = (uint32_t)mem;
    r->NDTR = count;

    uint32_t cr = 0;
    cr |= ((uint32_t)dir        << DMA_SxCR_DIR_Pos);   /* DIR[7:6] */
    cr |= (periph_inc ? DMA_SxCR_PINC : 0);             /* PINC */
    cr |= (mem_inc    ? DMA_SxCR_MINC : 0);             /* MINC */
    cr |= ((uint32_t)size       << DMA_SxCR_PSIZE_Pos); /* PSIZE[12:11] */
    cr |= ((uint32_t)size       << DMA_SxCR_MSIZE_Pos); /* MSIZE[14:13] */
    cr |= ((uint32_t)prio       << DMA_SxCR_PL_Pos);    /* PL[17:16] */
    cr |= (((uint32_t)channel & 0x7U) << DMA_SxCR_CHSEL_Pos); /* CHSEL[27:25] */
    /* DBM/CIRC/PFCTRL left 0; EN left 0 (start() arms it). */
    r->CR = cr;
}

void dma_hal_stream_enable_irq(dma_hal_stream_t *s, int tc, int te)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);
    if (tc) r->CR |= DMA_SxCR_TCIE; else r->CR &= ~DMA_SxCR_TCIE;
    if (te) r->CR |= DMA_SxCR_TEIE; else r->CR &= ~DMA_SxCR_TEIE;
}

void dma_hal_stream_disable_irq(dma_hal_stream_t *s)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);
    r->CR &= ~(DMA_SxCR_TCIE | DMA_SxCR_TEIE);
}

void dma_hal_stream_start(dma_hal_stream_t *s)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);
    r->CR |= DMA_SxCR_EN;     /* for M2M the transfer begins immediately */
}

void dma_hal_stream_stop(dma_hal_stream_t *s)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);
    r->CR &= ~DMA_SxCR_EN;
}

int dma_hal_stream_tc(dma_hal_stream_t *s)
{
    volatile uint32_t *risr = s->is_high ? &s->dma->HISR : &s->dma->LISR;
    return (int)((*risr >> (s->shift + 5U)) & 1U);
}

int dma_hal_stream_te(dma_hal_stream_t *s)
{
    volatile uint32_t *risr = s->is_high ? &s->dma->HISR : &s->dma->LISR;
    return (int)((*risr >> (s->shift + 3U)) & 1U);
}

void dma_hal_stream_clear_flags(dma_hal_stream_t *s)
{
    /* FE(+0) DME(+2) TE(+3) HT(+4) TC(+5) — write 1 to clear in LIFCR/HIFCR. */
    uint32_t mask = (1UL << (s->shift + 0U))
                  | (1UL << (s->shift + 2U))
                  | (1UL << (s->shift + 3U))
                  | (1UL << (s->shift + 4U))
                  | (1UL << (s->shift + 5U));
    if (s->is_high) s->dma->HIFCR = mask;
    else            s->dma->LIFCR = mask;
}

uint32_t dma_hal_stream_remaining(dma_hal_stream_t *s)
{
    return stream_reg(s->dma, s->idx)->NDTR;
}

irq_id_t dma_hal_stream_irq_id(dma_hal_stream_t *s)
{
    /* DMA1 streams -> IRQn 11..18 ; DMA2 streams -> IRQn 56..63. */
    int base = (s->ctlr == 1) ? 11 : 56;
    return (irq_id_t)(base + (int)s->idx);
}

int dma_hal_is_m2m_capable(dma_hal_stream_t *s)
{
    /* Only DMA2 on STM32F4 can perform memory-to-memory transfers. */
    return (s->ctlr == 2) ? 1 : 0;
}
