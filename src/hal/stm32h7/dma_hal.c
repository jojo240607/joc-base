#include "dma_hal.h"
#include "stm32h750xx.h"        /* DMA_CCR_*, DMAMUX_CxCR_*, RCC_AHB1ENR_* */
#include <stdlib.h>
#include <string.h>

/*
 * STM32H750 DMA HAL — dual layout.
 *
 * Silicon (STM32H750VB): DMA1/2 are CHANNEL-based. Each channel is a 5-register
 * block (CCR/CNDTR/CPAR/CM0AR/CM1AR) at stride 0x20 from +0x20. The channel
 * index here corresponds to the DMA stream index in the driver layer. There is
 * no M2M on DMA1/2 (MDMA only). Each peripheral selects its request via the
 * DMAMUX1 request-ID register (CxCR.DMAREQ_ID = 7-bit ID from RM0433).
 *
 * Renode (JOC_RENODE): The STM32H750B_DK's STM32DMA model exposes an F4-style
 * stream layout (CR/NDTR/PAR/M0AR/M1AR/FCR) at stride 0x18 from +0x10. The
 * flag registers (LISR/HISR/LIFCR/HIFCR) are identical to silicon. DMAMUX
 * registers are NOT mapped — writes to 0x40020800 cause a bus fault on Renode,
 * so DMAMUX access is guarded by #ifndef JOC_RENODE. M2M is available (DMA2
 * only, same as F4) because Renode's DMA model supports it natively.
 *
 * Flag bit positions per channel/stream group (same for both layouts, 6-bit
 * groups at shifts {0,6,16,22} for ch/stream {0,1,2,3} → LISR, {4,5,6,7} →
 * HISR):
 *   FE=+0, DME=+2, TE=+3, HT=+4, TC=+5
 * These match the DMA_FLAG_* defines in stm32h750xx.h.
 */

/* ---- channel/stream register access macros ---- */

/* Silicon: H7 channel registers (unit of the individual channel, stride 0x20). */
#if !defined(JOC_RENODE)
#define H7_CCR(base, idx)   (*((volatile uint32_t *)((uint8_t *)(base) + 0x20UL + 0x20UL * (idx))))
#define H7_CNDTR(base, idx) (*((volatile uint32_t *)((uint8_t *)(base) + 0x24UL + 0x20UL * (idx))))
#define H7_CPAR(base, idx)  (*((volatile uint32_t *)((uint8_t *)(base) + 0x28UL + 0x20UL * (idx))))
#define H7_CM0AR(base, idx) (*((volatile uint32_t *)((uint8_t *)(base) + 0x2CUL + 0x20UL * (idx))))
#define H7_CM1AR(base, idx) (*((volatile uint32_t *)((uint8_t *)(base) + 0x30UL + 0x20UL * (idx))))
#else
/* Renode: F4-style stream registers (stride 0x18). */
#define F4_CR(base, idx)    (*((volatile uint32_t *)((uint8_t *)(base) + 0x10UL + 0x18UL * (idx))))
#define F4_NDTR(base, idx)  (*((volatile uint32_t *)((uint8_t *)(base) + 0x14UL + 0x18UL * (idx))))
#define F4_PAR(base, idx)   (*((volatile uint32_t *)((uint8_t *)(base) + 0x18UL + 0x18UL * (idx))))
#define F4_M0AR(base, idx)  (*((volatile uint32_t *)((uint8_t *)(base) + 0x1CUL + 0x18UL * (idx))))
#define F4_M1AR(base, idx)  (*((volatile uint32_t *)((uint8_t *)(base) + 0x20UL + 0x18UL * (idx))))
#define F4_FCR(base, idx)   (*((volatile uint32_t *)((uint8_t *)(base) + 0x24UL + 0x18UL * (idx))))

/* Private Renode CR bit defines (device header is H7-accurate). */
#define RENODE_SxCR_EN         0x00000001U
#define RENODE_SxCR_DMEIE      0x00000002U
#define RENODE_SxCR_TEIE       0x00000004U
#define RENODE_SxCR_HTIE       0x00000008U
#define RENODE_SxCR_TCIE       0x00000010U
#define RENODE_SxCR_PFCTRL     0x00000020U
#define RENODE_SxCR_DIR_Pos    6U
#define RENODE_SxCR_DIR_Msk    (0x3UL << RENODE_SxCR_DIR_Pos)
#define RENODE_SxCR_CIRC       0x00000100U
#define RENODE_SxCR_PINC       0x00000200U
#define RENODE_SxCR_MINC       0x00000400U
#define RENODE_SxCR_PSIZE_Pos  11U
#define RENODE_SxCR_PSIZE_Msk  (0x3UL << RENODE_SxCR_PSIZE_Pos)
#define RENODE_SxCR_MSIZE_Pos  13U
#define RENODE_SxCR_MSIZE_Msk  (0x3UL << RENODE_SxCR_MSIZE_Pos)
#define RENODE_SxCR_PL_Pos     16U
#define RENODE_SxCR_PL_Msk     (0x3UL << RENODE_SxCR_PL_Pos)
#define RENODE_SxCR_CHSEL_Pos  25U
#define RENODE_SxCR_CHSEL_Msk  (0x7UL << RENODE_SxCR_CHSEL_Pos)
#endif

/* ---- flag register access (identical silicon / Renode) ---- */
#define F_LISR(base)  (((volatile uint32_t *)(base))[0])
#define F_HISR(base)  (((volatile uint32_t *)(base))[1])
#define F_LIFCR(base) (((volatile uint32_t *)(base))[2])
#define F_HIFCR(base) (((volatile uint32_t *)(base))[3])

/* ---- opaque handle ---- */
struct dma_hal_stream {
    volatile uint32_t *dma;  /* DMA1_BASE or DMA2_BASE, typed as uint32_t* */
    uint32_t           idx;      /* 0..7 */
    uint32_t           shift;    /* flag bit offset within L/H ISR/FCR */
    int                is_high;  /* idx>=4 => HISR/HIFCR */
    int                ctlr;     /* 1 = DMA1, 2 = DMA2 */
};

static const uint8_t dma_fsr_shift[4] = { 0, 6, 16, 22 };

dma_hal_stream_t *dma_hal_stream_create(void *dma_periph, uint32_t stream_idx)
{
    dma_hal_stream_t *s = (dma_hal_stream_t *)malloc(sizeof(dma_hal_stream_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->dma     = (volatile uint32_t *)dma_periph;
    s->idx     = stream_idx & 0x7U;
    s->shift   = dma_fsr_shift[s->idx % 4U];
    s->is_high = (s->idx >= 4U) ? 1 : 0;
    s->ctlr    = ((uint32_t)dma_periph == DMA1_BASE) ? 1 : 2;
    return s;
}

void dma_hal_stream_destroy(dma_hal_stream_t *s)
{
    if (s) free(s);
}

void dma_hal_enable_clock(void *dma_periph)
{
#if !defined(JOC_RENODE)
    /* Silicon: enable DMAMUX1 clock too (shared by both DMA controllers). */
    RCC->AHB1ENR |= RCC_AHB1ENR_DMAMUX1EN;
#endif
    if ((uint32_t)dma_periph == DMA1_BASE)
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    else
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
}

void dma_hal_stream_config(dma_hal_stream_t *s, dma_hal_dir_t dir, uint32_t channel,
                           const void *periph, void *mem, uint32_t count,
                           dma_hal_size_t size, int periph_inc, int mem_inc,
                           uint32_t prio)
{
#if !defined(JOC_RENODE)
    /* ---- Silicon (H7 channel) ---- */
    volatile uint32_t *ccr   = &H7_CCR(s->dma, s->idx);
    volatile uint32_t *cndtr = &H7_CNDTR(s->dma, s->idx);
    volatile uint32_t *cpar  = &H7_CPAR(s->dma, s->idx);
    volatile uint32_t *cm0ar = &H7_CM0AR(s->dma, s->idx);
    /* CM1AR unused (double-buffer not supported yet). */

    /* CR may only be written while EN=0. */
    *ccr &= ~DMA_CCR_EN;
    while (*ccr & DMA_CCR_EN) { }

    /* Clear stale flags before reprogramming. */
    dma_hal_stream_clear_flags(s);

    /* Program data-path registers (CNDTR writable only while EN=0). */
    *cpar  = (uint32_t)periph;
    *cm0ar = (uint32_t)mem;
    *cndtr = count;

    uint32_t cr = 0;
    /* DIR is 1-bit on H7 DMA1/2: 0=P2M, 1=M2P.  DIR=0x40 matches the define. */
    if (dir == DMA_HAL_DIR_M2P) cr |= DMA_CCR_DIR;
    cr |= (periph_inc ? DMA_CCR_PINC : 0);
    cr |= (mem_inc    ? DMA_CCR_MINC : 0);
    cr |= ((uint32_t)size << DMA_CCR_PSIZE_Pos);
    cr |= ((uint32_t)size << DMA_CCR_MSIZE_Pos);
    cr |= ((uint32_t)prio << DMA_CCR_PL_Pos);
    /* MBURST/PBURST/TRBUFF/DBM/CT left at 0. */
    *ccr = cr;

    /* Program DMAMUX channel to select the peripheral request source.
     * DMAMUX1 channel index = DMA stream index (fixed 1:1 mapping).
     * The `channel` parameter is the 7-bit DMAMUX request ID from RM0433
     * (e.g. USART1_TX=37, USART1_RX=38, etc.).
     * For peripheral transfers (P2M/M2P) the request ID is >0; any request
     * may also be programmed with a zero DMAREQ_ID (idle, no transfer). */
    volatile uint32_t *dmamux_ccr = (volatile uint32_t *)(DMAMUX1_BASE + 4u * s->idx);
    *dmamux_ccr = (channel & 0x7Fu) << DMAMUX_CxCR_DMAREQ_ID_Pos;
    /* SOIE=0, EGE=0 (no event generation). */

#else
    /* ---- Renode (F4-style stream) ---- */
    volatile uint32_t *cr   = &F4_CR(s->dma, s->idx);
    volatile uint32_t *ndtr = &F4_NDTR(s->dma, s->idx);
    volatile uint32_t *par  = &F4_PAR(s->dma, s->idx);
    volatile uint32_t *m0ar = &F4_M0AR(s->dma, s->idx);
    /* FCR unused. */

    /* Clear EN and wait. */
    *cr &= ~RENODE_SxCR_EN;
    while (*cr & RENODE_SxCR_EN) { }

    dma_hal_stream_clear_flags(s);

    *par  = (uint32_t)periph;
    *m0ar = (uint32_t)mem;
    *ndtr = count;

    uint32_t c = 0;
    c |= ((uint32_t)dir << RENODE_SxCR_DIR_Pos);
    c |= (periph_inc ? RENODE_SxCR_PINC : 0);
    c |= (mem_inc    ? RENODE_SxCR_MINC : 0);
    c |= ((uint32_t)size << RENODE_SxCR_PSIZE_Pos);
    c |= ((uint32_t)size << RENODE_SxCR_MSIZE_Pos);
    c |= ((uint32_t)prio << RENODE_SxCR_PL_Pos);
    c |= ((channel & 0x7U) << RENODE_SxCR_CHSEL_Pos);
    /* DMAMUX NOT programmed (not mapped in Renode). */
    *cr = c;
#endif
}

void dma_hal_stream_enable_irq(dma_hal_stream_t *s, int tc, int te)
{
#if !defined(JOC_RENODE)
    volatile uint32_t *ccr = &H7_CCR(s->dma, s->idx);
    if (tc) *ccr |= DMA_CCR_TCIE;  else *ccr &= ~DMA_CCR_TCIE;
    if (te) *ccr |= DMA_CCR_TEIE;  else *ccr &= ~DMA_CCR_TEIE;
#else
    volatile uint32_t *cr = &F4_CR(s->dma, s->idx);
    if (tc) *cr |= RENODE_SxCR_TCIE;  else *cr &= ~RENODE_SxCR_TCIE;
    if (te) *cr |= RENODE_SxCR_TEIE;  else *cr &= ~RENODE_SxCR_TEIE;
#endif
}

void dma_hal_stream_disable_irq(dma_hal_stream_t *s)
{
#if !defined(JOC_RENODE)
    volatile uint32_t *ccr = &H7_CCR(s->dma, s->idx);
    *ccr &= ~(DMA_CCR_TCIE | DMA_CCR_TEIE);
#else
    volatile uint32_t *cr = &F4_CR(s->dma, s->idx);
    *cr &= ~(RENODE_SxCR_TCIE | RENODE_SxCR_TEIE);
#endif
}

void dma_hal_stream_set_circular(dma_hal_stream_t *s, int en)
{
#if !defined(JOC_RENODE)
    volatile uint32_t *ccr = &H7_CCR(s->dma, s->idx);
    *ccr &= ~DMA_CCR_EN;
    while (*ccr & DMA_CCR_EN) { }
    if (en) *ccr |= DMA_CCR_CIRC;  else *ccr &= ~DMA_CCR_CIRC;
#else
    volatile uint32_t *cr = &F4_CR(s->dma, s->idx);
    *cr &= ~RENODE_SxCR_EN;
    while (*cr & RENODE_SxCR_EN) { }
    if (en) *cr |= RENODE_SxCR_CIRC;  else *cr &= ~RENODE_SxCR_CIRC;
#endif
}

void dma_hal_stream_start(dma_hal_stream_t *s)
{
#if !defined(JOC_RENODE)
    H7_CCR(s->dma, s->idx) |= DMA_CCR_EN;
#else
    F4_CR(s->dma, s->idx) |= RENODE_SxCR_EN;
#endif
}

void dma_hal_stream_stop(dma_hal_stream_t *s)
{
#if !defined(JOC_RENODE)
    H7_CCR(s->dma, s->idx) &= ~DMA_CCR_EN;
#else
    F4_CR(s->dma, s->idx) &= ~RENODE_SxCR_EN;
#endif
}

/* ---- flag operations (identical for both layouts) ---- */
int dma_hal_stream_tc(dma_hal_stream_t *s)
{
    volatile uint32_t *risr = s->is_high ? &F_HISR(s->dma) : &F_LISR(s->dma);
    return (int)((*risr >> (s->shift + DMA_FLAG_TC)) & 1U);
}

int dma_hal_stream_te(dma_hal_stream_t *s)
{
    volatile uint32_t *risr = s->is_high ? &F_HISR(s->dma) : &F_LISR(s->dma);
    return (int)((*risr >> (s->shift + DMA_FLAG_TE)) & 1U);
}

void dma_hal_stream_clear_flags(dma_hal_stream_t *s)
{
    uint32_t mask = (1UL << (s->shift + DMA_FLAG_FE))
                  | (1UL << (s->shift + DMA_FLAG_DME))
                  | (1UL << (s->shift + DMA_FLAG_TE))
                  | (1UL << (s->shift + DMA_FLAG_HT))
                  | (1UL << (s->shift + DMA_FLAG_TC));
    if (s->is_high) F_HIFCR(s->dma) = mask;
    else            F_LIFCR(s->dma) = mask;
}

uint32_t dma_hal_stream_remaining(dma_hal_stream_t *s)
{
#if !defined(JOC_RENODE)
    return H7_CNDTR(s->dma, s->idx);
#else
    return F4_NDTR(s->dma, s->idx);
#endif
}

irq_id_t dma_hal_stream_irq_id(dma_hal_stream_t *s)
{
    /* H7 DMA IRQn layout is identical to F4 (same STM32 core IRQ assignment
     * for DMA1/2 Stream0..7). Verified against stm32h750xx.h IRQn_Type. */
    static const uint8_t dma1_irq[8] = { 11, 12, 13, 14, 15, 16, 17, 47 };
    static const uint8_t dma2_irq[8] = { 56, 57, 58, 59, 60, 68, 69, 70 };
    return (irq_id_t)((s->ctlr == 1) ? dma1_irq[s->idx] : dma2_irq[s->idx]);
}

int dma_hal_is_m2m_capable(dma_hal_stream_t *s)
{
#if !defined(JOC_RENODE)
    /* H7 DMA1/2 have no M2M mode (MDMA handles it). */
    (void)s;
    return 0;
#else
    /* Renode STM32DMA model: DMA2 supports M2M (same as F4). */
    return (s->ctlr == 2) ? 1 : 0;
#endif
}

/* ========================================================================
 * H7 DMAMUX1 route table (fixed assignment, RM0433 request IDs).
 * DMAMUX1 allows any channel to carry any request (full flexibility), but
 * we assign fixed streams for deterministic CM debug console behavior.
 * The `channel` field in the return value is the 7-bit DMAMUX request ID;
 * the silicon config path writes it into DMAMUX_CxCR.DMAREQ_ID. Under
 * Renode the channel is used as CHSEL (3 bits) — harmless mismatch since
 * Renode ignores DMAMUX entirely and routes by stream index.
 * ===================================================================== */
dma_route_t dma_hal_route(dma_req_id_t req)
{
    switch (req) {
    /* ---- USART / UART (console: USART1 fixed on dma1 s0/s1) ---- */
    case DMA_REQ_USART1_TX: return (dma_route_t){ "dma1", 0, 37 };
    case DMA_REQ_USART1_RX: return (dma_route_t){ "dma1", 1, 38 };
    case DMA_REQ_USART2_TX: return (dma_route_t){ "dma1", 2, 39 };
    case DMA_REQ_USART2_RX: return (dma_route_t){ "dma1", 3, 40 };
    case DMA_REQ_USART3_TX: return (dma_route_t){ "dma1", 4, 41 };
    case DMA_REQ_USART3_RX: return (dma_route_t){ "dma1", 5, 42 };
    case DMA_REQ_UART4_TX:  return (dma_route_t){ "dma1", 6, 43 };
    case DMA_REQ_UART4_RX:  return (dma_route_t){ "dma1", 7, 44 };
    case DMA_REQ_UART5_TX:  return (dma_route_t){ "dma2", 0, 45 };
    case DMA_REQ_UART5_RX:  return (dma_route_t){ "dma2", 1, 46 };
    case DMA_REQ_USART6_TX: return (dma_route_t){ "dma2", 2, 47 };
    case DMA_REQ_USART6_RX: return (dma_route_t){ "dma2", 3, 48 };
    /* ---- SPI ---- */
    case DMA_REQ_SPI1_TX:   return (dma_route_t){ "dma2", 4, 49 };
    case DMA_REQ_SPI1_RX:   return (dma_route_t){ "dma2", 5, 50 };
    case DMA_REQ_SPI2_TX:   return (dma_route_t){ "dma2", 6, 51 };
    case DMA_REQ_SPI2_RX:   return (dma_route_t){ "dma2", 7, 52 };
    case DMA_REQ_SPI3_TX:   return (dma_route_t){ "dma1", 2, 53 };  /* shares USART2_TX */
    case DMA_REQ_SPI3_RX:   return (dma_route_t){ "dma1", 3, 54 };  /* shares USART2_RX */
    /* ---- I2C ---- */
    case DMA_REQ_I2C1_TX:   return (dma_route_t){ "dma1", 4, 65 };  /* shares USART3_TX */
    case DMA_REQ_I2C1_RX:   return (dma_route_t){ "dma1", 5, 66 };  /* shares USART3_RX */
    case DMA_REQ_I2C2_TX:   return (dma_route_t){ "dma1", 6, 67 };  /* shares UART4_TX */
    case DMA_REQ_I2C2_RX:   return (dma_route_t){ "dma1", 7, 68 };  /* shares UART4_RX */
    case DMA_REQ_I2C3_TX:   return (dma_route_t){ "dma2", 4, 69 };  /* shares SPI1_TX */
    case DMA_REQ_I2C3_RX:   return (dma_route_t){ "dma2", 5, 70 };  /* shares SPI1_RX */
    /* ---- ADC ---- */
    case DMA_REQ_ADC1:      return (dma_route_t){ "dma2", 4, 1 };
    case DMA_REQ_ADC2:      return (dma_route_t){ "dma2", 5, 2 };
    case DMA_REQ_ADC3:      return (dma_route_t){ "dma2", 6, 99 };
    /* ---- TIMER update events ---- */
    case DMA_REQ_TIM2_UP:   return (dma_route_t){ "dma1", 6, 14 };
    case DMA_REQ_TIM3_UP:   return (dma_route_t){ "dma1", 7, 19 };
    case DMA_REQ_TIM4_UP:   return (dma_route_t){ "dma2", 0, 24 };
    case DMA_REQ_TIM5_UP:   return (dma_route_t){ "dma2", 1, 29 };
    case DMA_REQ_TIM6_UP:   return (dma_route_t){ "dma2", 2, 30 };
    case DMA_REQ_TIM7_UP:   return (dma_route_t){ "dma2", 3, 31 };
    case DMA_REQ_TIM8_UP:   return (dma_route_t){ "dma1", 2, 36 };
    /* ---- DAC / SDIO (not yet mapped for H750) ---- */
    case DMA_REQ_DAC1:
    case DMA_REQ_DAC2:
    case DMA_REQ_SDIO:
    default:
        return (dma_route_t){ NULL, 0, 0 };
    }
}