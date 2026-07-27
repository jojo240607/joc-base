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
 * DME=+2, TE=+3, HT=+4, TC=+5. The 6-bit groups are NOT contiguous:
 * stream0/1 -> bit base 0/6, stream2/3 -> bit base 16/22 (a 4-bit hole
 * sits between stream1->2 and stream3->4). base = dma_fsr_shift[idx%4]
 * = {0,6,16,22} (HISR/HIFCR reuse the same layout for stream4..7).
 */

struct dma_hal_stream {
    DMA_TypeDef  *dma;     /* DMA1 or DMA2 base */
    uint32_t      idx;     /* 0..7 */
    uint32_t      shift;   /* dma_fsr_shift[idx%4] — flag bit offset within L/H ISR/FCR */
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
    static const uint8_t dma_fsr_shift[4] = { 0, 6, 16, 22 };
    s->shift   = dma_fsr_shift[s->idx % 4U];
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

void dma_hal_stream_set_circular(dma_hal_stream_t *s, int en)
{
    DMA_Stream_TypeDef *r = stream_reg(s->dma, s->idx);
    /* CIRC may only be written while EN=0: clear EN and wait for the hardware to
     * acknowledge before modifying it. */
    r->CR &= ~DMA_SxCR_EN;
    while (r->CR & DMA_SxCR_EN) { }
    if (en) r->CR |= DMA_SxCR_CIRC;
    else    r->CR &= ~DMA_SxCR_CIRC;
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
    /* STM32F4 DMA IRQn layout is NON-contiguous: there are gaps between the
     * low streams and the high ones (Ethernet/CAN2/OTG-FS sit between
     * DMA2_Stream4 and Stream5; many peripherals sit between DMA1_Stream6 and
     * Stream7). A naive `base + idx` is WRONG for the high streams and makes the
     * completion ISR arm the wrong NVIC line — the real DMA stream interrupt
     * stays disabled, so wait_done() times out even though the transfer data is
     * correct. Use an explicit per-stream table (verified against the vector
     * table in startup_stm32f407xx.s):
     *   DMA1: 11,12,13,14,15,16,17,47
     *   DMA2: 56,57,58,59,60,68,69,70 */
    static const uint8_t dma1_irq[8] = { 11, 12, 13, 14, 15, 16, 17, 47 };
    static const uint8_t dma2_irq[8] = { 56, 57, 58, 59, 60, 68, 69, 70 };
    return (irq_id_t)((s->ctlr == 1) ? dma1_irq[s->idx] : dma2_irq[s->idx]);
}

int dma_hal_is_m2m_capable(dma_hal_stream_t *s)
{
    /* Only DMA2 on STM32F4 can perform memory-to-memory transfers. */
    return (s->ctlr == 2) ? 1 : 0;
}

/* STM32F4 DMA request routing (RM0090 Table 30/31). Each peripheral request is
 * hard-wired to exactly one (controller, stream, channel). Returns the device-
 * manager name of the owning controller plus the concrete stream + CHSEL.
 * Entries not yet mapped (or not applicable) return name=NULL so a driver that
 * asks for them refuses cleanly instead of programming a wrong stream. */
dma_route_t dma_hal_route(dma_req_id_t req)
{
    switch (req) {
    /* ---- USART / UART (DMA channel 4) ---- */
    case DMA_REQ_USART1_TX: return (dma_route_t){ "dma2", 7, 4 };
    case DMA_REQ_USART1_RX: return (dma_route_t){ "dma2", 5, 4 };
    case DMA_REQ_USART2_TX: return (dma_route_t){ "dma1", 6, 4 };
    case DMA_REQ_USART2_RX: return (dma_route_t){ "dma1", 5, 4 };
    case DMA_REQ_USART3_TX: return (dma_route_t){ "dma1", 3, 4 };
    case DMA_REQ_USART3_RX: return (dma_route_t){ "dma1", 1, 4 };
    case DMA_REQ_UART4_TX:  return (dma_route_t){ "dma1", 4, 4 };
    case DMA_REQ_UART4_RX:  return (dma_route_t){ "dma1", 2, 4 };
    case DMA_REQ_UART5_TX:  return (dma_route_t){ "dma1", 7, 4 };
    case DMA_REQ_UART5_RX:  return (dma_route_t){ "dma1", 0, 4 };
    /* ---- SPI / I2S (I2S2=SPI2, I2S3=SPI3) ----
     * Channel is the DMA request multiplexer select (CHSEL). On STM32F4 the
     * SPI/I2S request lines are wired to CHANNEL 0 (NOT 3 — that is SPI1's
     * DMA2 channel, a common copy-paste error). Streams are per RM0090
     * Table 30/31: SPI2=I2S2 -> TX DMA1_Stream4 / RX DMA1_Stream3;
     * SPI3=I2S3 -> TX DMA1_Stream5 / RX DMA1_Stream2. */
    case DMA_REQ_SPI1_TX:   return (dma_route_t){ "dma2", 3, 3 };
    case DMA_REQ_SPI1_RX:   return (dma_route_t){ "dma2", 2, 3 };
    case DMA_REQ_SPI2_TX:   return (dma_route_t){ "dma1", 4, 0 };   /* I2S2 TX */
    case DMA_REQ_SPI2_RX:   return (dma_route_t){ "dma1", 3, 0 };   /* I2S2 RX */
    case DMA_REQ_SPI3_TX:   return (dma_route_t){ "dma1", 5, 0 };   /* I2S3 TX */
    case DMA_REQ_SPI3_RX:   return (dma_route_t){ "dma1", 2, 0 };   /* I2S3 RX */
    /* ---- ADC (DMA channel 0 on DMA2) ---- */
    case DMA_REQ_ADC1:      return (dma_route_t){ "dma2", 0, 0 };
    case DMA_REQ_ADC2:      return (dma_route_t){ "dma2", 2, 1 };
    case DMA_REQ_ADC3:      return (dma_route_t){ "dma2", 1, 2 };
    /* ---- DAC (DMA channel 7 on DMA1; TIM6_UP/TIM7_UP share the stream) ---- */
    case DMA_REQ_DAC1:      return (dma_route_t){ "dma1", 5, 7 };
    case DMA_REQ_DAC2:      return (dma_route_t){ "dma1", 6, 7 };
    /* ---- I2C (DMA1 only) — the F1-style I2C on F4 still exposes DMA requests
     * (CR2.DMAEN). Routes per RM0090 Table 30. TX drives DR from memory (M2P),
     * RX fills memory from DR (P2M); the driver acquires both streams. */
    case DMA_REQ_I2C1_TX:  return (dma_route_t){ "dma1", 6, 1 };
    case DMA_REQ_I2C1_RX:  return (dma_route_t){ "dma1", 0, 1 };
    case DMA_REQ_I2C2_TX:  return (dma_route_t){ "dma1", 7, 1 };
    case DMA_REQ_I2C2_RX:  return (dma_route_t){ "dma1", 2, 1 };
    case DMA_REQ_I2C3_TX:  return (dma_route_t){ "dma1", 4, 3 };
    case DMA_REQ_I2C3_RX:  return (dma_route_t){ "dma1", 2, 3 };
    /* ---- SDIO (DMA2, channel 4) ----
     * The SDIO host has a SINGLE DMA request line; the transfer direction is
     * selected by DCTRL.DTDIR, so the driver reconfigures the one acquired stream
     * per transfer (read = P2M, write = M2P). Both DMA2 Stream3 and Stream6 carry
     * the SDIO request (channel 4); we pick Stream6. */
    case DMA_REQ_SDIO:     return (dma_route_t){ "dma2", 6, 4 };
    /* ---- TIMER update events (TIMx_UP) ----
     * Each TIM's overflow is a DMA request. Routes per RM0090 Table 30/31
     * (the TIM2_UP mapping is verified on hardware by the timer DMA self-test).
     * TIM6_UP/TIM7_UP SHARE the DAC channel request lines (DAC1_CH1 / DAC2_CH1),
     * so they must not be used while the DAC is streaming on the same request. */
    case DMA_REQ_TIM2_UP:  return (dma_route_t){ "dma1", 7, 3 };
    case DMA_REQ_TIM3_UP:  return (dma_route_t){ "dma1", 2, 5 };
    case DMA_REQ_TIM4_UP:  return (dma_route_t){ "dma1", 3, 3 };
    case DMA_REQ_TIM5_UP:  return (dma_route_t){ "dma1", 0, 7 };
    case DMA_REQ_TIM6_UP:  return (dma_route_t){ "dma1", 1, 6 };
    case DMA_REQ_TIM7_UP:  return (dma_route_t){ "dma1", 2, 6 };
    case DMA_REQ_TIM8_UP:  return (dma_route_t){ "dma2", 1, 0 };
    default:
        return (dma_route_t){ NULL, 0, 0 };
    }
}
