#include <stdlib.h>
#include <string.h>
#include <stm32h750xx.h>   /* TIM_TypeDef, RCC, IRQn_Type — board/chip layer only */
#include "tim_hal.h"

/*
 * STM32H750 general-purpose TIMER HAL.
 *
 * H750 has TIM1/TIM8 (advanced: RCR + BDTR + complementary outputs) on APB2,
 * TIM2..TIM7 (TIM6/TIM7 are basic — no CC channels) and TIM12..TIM14 on APB1,
 * TIM15..TIM17 on APB2. Register layout is F4-compatible, so this is a near
 * verbatim port of the F1 implementation with the H750 clock gates / IRQ map.
 *
 * The concrete handle holds only the TIM_TypeDef*; the driver never touches it.
 * The driver asks this HAL to: enable the peripheral clock, compute PSC/ARR for
 * a target overflow rate, start/stop counting, and manage the update interrupt.
 */

/* TIM6/TIM7 are basic timers with no CC channels (PWM not supported). */
#define IS_BASIC_TIM(t)  ((t) == TIM6 || (t) == TIM7)

struct tim_hal_handle {
    TIM_TypeDef *tim;
    int advanced;   /* 1 for TIM1/TIM8 (RCR + BDTR/complementary/break) */
};

tim_hal_handle_t *tim_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    tim_hal_handle_t *h = (tim_hal_handle_t *)malloc(sizeof(tim_hal_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->tim = (TIM_TypeDef *)peripheral;
    /* TIM1 and TIM8 are the advanced timers on H7. */
    h->advanced = (h->tim == TIM1 || h->tim == TIM8) ? 1 : 0;
    return h;
}

void tim_hal_destroy(tim_hal_handle_t *h)
{
    if (!h) return;
    h->tim->CR1 &= ~TIM_CR1_CEN;   /* stop counting before freeing */
    free(h);
}

/* H7 timer clock gates:
 *  - APB2ENR: TIM1, TIM8, TIM15..TIM17
 *  - APB1LENR: TIM2..TIM7, TIM12..TIM14 */
static void tim_hal_clock_on(TIM_TypeDef *t)
{
    if (t == TIM1)      RCC->APB2ENR  |= RCC_APB2ENR_TIM1EN;
    else if (t == TIM8) RCC->APB2ENR  |= RCC_APB2ENR_TIM8EN;
    else if (t == TIM15) RCC->APB2ENR |= RCC_APB2ENR_TIM15EN;
    else if (t == TIM16) RCC->APB2ENR |= RCC_APB2ENR_TIM16EN;
    else if (t == TIM17) RCC->APB2ENR |= RCC_APB2ENR_TIM17EN;
    else if (t == TIM2)  RCC->APB1LENR |= RCC_APB1LENR_TIM2EN;
    else if (t == TIM3)  RCC->APB1LENR |= RCC_APB1LENR_TIM3EN;
    else if (t == TIM4)  RCC->APB1LENR |= RCC_APB1LENR_TIM4EN;
    else if (t == TIM5)  RCC->APB1LENR |= RCC_APB1LENR_TIM5EN;
    else if (t == TIM6)  RCC->APB1LENR |= RCC_APB1LENR_TIM6EN;
    else if (t == TIM7)  RCC->APB1LENR |= RCC_APB1LENR_TIM7EN;
    else if (t == TIM12) RCC->APB1LENR |= RCC_APB1LENR_TIM12EN;
    else if (t == TIM13) RCC->APB1LENR |= RCC_APB1LENR_TIM13EN;
    else if (t == TIM14) RCC->APB1LENR |= RCC_APB1LENR_TIM14EN;
}

void tim_hal_enable_clock(tim_hal_handle_t *h)
{
    if (!h) return;
    tim_hal_clock_on(h->tim);
    (void)h->tim->CNT;   /* dummy read so the clock is active before we touch regs */
}

void tim_hal_config(tim_hal_handle_t *h, uint32_t timer_clk_hz, uint32_t tick_hz)
{
    if (!h || tick_hz == 0 || timer_clk_hz < tick_hz) return;
    TIM_TypeDef *t = h->tim;

    /* Total count ratio = number of timer clocks between two overflow events. */
    uint64_t total = (uint64_t)timer_clk_hz / (uint64_t)tick_hz;
    if (total == 0) total = 1;

    /* Pick a prescaler so ARR fits the 16-bit auto-reload register:
     *   presc = ceil(total / 65536) - 1,  then  ARR = total / (presc + 1) - 1
     * (period = (PSC+1) * (ARR+1) timer clocks). */
    uint32_t presc = (uint32_t)((total - 1) / 65536UL);
    uint32_t arr   = (uint32_t)(total / (uint64_t)(presc + 1));  /* = ARR + 1 */
    if (arr == 0) arr = 1;

    t->CR1 = 0;                  /* stop, reset control defaults (up-count) */
    t->PSC  = presc;
    t->ARR  = arr - 1U;
    t->EGR  = TIM_EGR_UG;        /* load PSC/ARR into the active registers */
    t->SR   = 0;                 /* clear UIF (set by the UG above) + any flag */
}

void tim_hal_start(tim_hal_handle_t *h)
{
    if (h) h->tim->CR1 |= TIM_CR1_CEN;
}

void tim_hal_stop(tim_hal_handle_t *h)
{
    if (h) h->tim->CR1 &= ~TIM_CR1_CEN;
}

/* Route the Update event to TRGO (CR2.MMS = 0b010). Used by the DAC driver to
 * clock a DMA burst: each timer overflow trips the DAC trigger, which moves
 * DHR->DOR and raises the DAC's DMA request. No ISR is involved. CR2 is left
 * otherwise untouched (tim_hal_config never writes CR2). */
void tim_hal_master_trgo_update(tim_hal_handle_t *h)
{
    if (!h) return;
    h->tim->CR2 = (h->tim->CR2 & ~TIM_CR2_MMS_Msk) | TIM_CR2_MMS_UPDATE;
}

uint32_t tim_hal_get_counter(tim_hal_handle_t *h)
{
    return h ? h->tim->CNT : 0U;
}

irq_id_t tim_hal_irq_id(tim_hal_handle_t *h)
{
    if (!h) return -1;
    TIM_TypeDef *t = h->tim;
    /* H750 vector table (differs from H743!). TIM8/TIM12, TIM8/TIM13 and
     * TIM8/TIM14 SHARE IRQ lines — the driver checks tim_hal_uif_pending()
     * before acting, so siblings on a shared line don't cross-trigger. */
    if (t == TIM1)   return (irq_id_t)TIM1_UP_IRQn;          /* IRQ 25 */
    if (t == TIM2)   return (irq_id_t)TIM2_IRQn;             /* IRQ 28 */
    if (t == TIM3)   return (irq_id_t)TIM3_IRQn;             /* IRQ 29 */
    if (t == TIM4)   return (irq_id_t)TIM4_IRQn;             /* IRQ 30 */
    if (t == TIM5)   return (irq_id_t)TIM5_IRQn;             /* IRQ 50 */
    if (t == TIM6)   return (irq_id_t)TIM6_DAC_IRQn;         /* IRQ 54 */
    if (t == TIM7)   return (irq_id_t)TIM7_IRQn;             /* IRQ 55 */
    if (t == TIM8)   return (irq_id_t)TIM8_UP_TIM13_IRQn;    /* IRQ 44 (shared w/ TIM13) */
    if (t == TIM12)  return (irq_id_t)TIM8_BRK_TIM12_IRQn;   /* IRQ 43 (shared w/ TIM8_BRK) */
    if (t == TIM13)  return (irq_id_t)TIM8_UP_TIM13_IRQn;    /* IRQ 44 (shared w/ TIM8_UP) */
    if (t == TIM14)  return (irq_id_t)TIM8_TRG_COM_TIM14_IRQn; /* IRQ 45 (shared w/ TIM8_TRG_COM) */
    if (t == TIM15)  return (irq_id_t)TIM15_IRQn;            /* IRQ 116 */
    if (t == TIM16)  return (irq_id_t)TIM16_IRQn;            /* IRQ 117 */
    if (t == TIM17)  return (irq_id_t)TIM17_IRQn;            /* IRQ 118 */
    return -1;
}

void tim_hal_enable_update_irq(tim_hal_handle_t *h)
{
    if (h) h->tim->DIER |= TIM_DIER_UIE;
}

void tim_hal_disable_update_irq(tim_hal_handle_t *h)
{
    if (h) h->tim->DIER &= ~TIM_DIER_UIE;
}

void tim_hal_clear_uif(tim_hal_handle_t *h)
{
    if (h) h->tim->SR &= ~TIM_SR_UIF;   /* rc_w0 on H7: write 0 to clear */
}

int tim_hal_uif_pending(tim_hal_handle_t *h)
{
    return (h && (h->tim->SR & TIM_SR_UIF)) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * TIMER DMA (Update-event burst). See tim_hal.h for the contract.
 * ------------------------------------------------------------------------- */
static volatile uint32_t *pwm_ccr(tim_hal_handle_t *h, int ch);

void tim_hal_dma_update_enable(tim_hal_handle_t *h)
{
    if (h) h->tim->DIER |= TIM_DIER_UDE;    /* Update DMA request enable */
}
void tim_hal_dma_update_disable(tim_hal_handle_t *h)
{
    if (h) h->tim->DIER &= ~TIM_DIER_UDE;   /* stop raising UP DMA requests */
}
volatile uint32_t *tim_hal_get_ccr(tim_hal_handle_t *h, int ch)
{
    return pwm_ccr(h, ch);                  /* &TIMx_CCRx — the DMA destination */
}
uint32_t tim_hal_get_arr(tim_hal_handle_t *h)
{
    return h ? (h->tim->ARR & 0xFFFFUL) : 0U;
}

/* ===========================================================================
 * PWM CHANNEL support — see tim_hal.h for the ownership/coordination contract.
 * =========================================================================== */

uint32_t tim_hal_pwm_set_period(tim_hal_handle_t *h, uint32_t timer_clk_hz,
                                uint32_t freq_hz)
{
    if (!h || freq_hz == 0 || timer_clk_hz < freq_hz) return 0;
    TIM_TypeDef *t = h->tim;

    /* Same PSC/ARR math as tim_hal_config: period = (PSC+1)*(ARR+1) clocks. */
    uint64_t total = (uint64_t)timer_clk_hz / (uint64_t)freq_hz;
    if (total == 0) total = 1;
    uint32_t presc = (uint32_t)((total - 1) / 65536UL);
    uint32_t arr   = (uint32_t)(total / (uint64_t)(presc + 1));  /* = ARR + 1 */
    if (arr == 0) arr = 1;

    t->PSC = presc;
    t->ARR = arr - 1U;
    t->EGR = TIM_EGR_UG;        /* load PSC/ARR into the active shadow registers */
    t->SR  = 0;                 /* clear UIF raised by the UG above */
    return arr;                 /* period in ticks = ARR + 1 */
}

static volatile uint32_t *pwm_ccr(tim_hal_handle_t *h, int ch)
{
    TIM_TypeDef *t = h->tim;
    switch (ch) {
        case 1: return &t->CCR1;
        case 2: return &t->CCR2;
        case 3: return &t->CCR3;
        case 4: return &t->CCR4;
        default: return NULL;
    }
}

/* TIM6/TIM7 are basic timers with NO CC channels — skip PWM config on those. */
void tim_hal_pwm_config_channel(tim_hal_handle_t *h, int ch, int mode, int polarity)
{
    if (!h || ch < 1 || ch > 4) return;
    TIM_TypeDef *t = h->tim;
    /* Skip basic timers (TIM6/TIM7) that have no CC channels. */
    if (IS_BASIC_TIM(t)) return;

    uint32_t ocm = (mode == 2) ? 0x7UL : 0x6UL;   /* OCxM: 110=PWM1, 111=PWM2 */

    if (ch <= 2) {
        /* CCMR1: CC1S/CC2S=00 (output), OCxPE=1 (preload), OCxM=pwm mode */
        if (ch == 1) {
            t->CCMR1 = (t->CCMR1 & ~0x00FFUL) |
                       (0x6UL << 4) | (ocm << 4);   /* OC1PE | OC1M */
            t->CCMR1 &= ~0x0003UL;                  /* CC1S = 00 (output) */
        } else {
            t->CCMR1 = (t->CCMR1 & ~0xFF00UL) |
                       (0x6UL << 12) | (ocm << 12); /* OC2PE | OC2M */
            t->CCMR1 &= ~0x0300UL;                  /* CC2S = 00 (output) */
        }
    } else {
        if (ch == 3) {
            t->CCMR2 = (t->CCMR2 & ~0x00FFUL) |
                       (0x6UL << 4) | (ocm << 4);   /* OC3PE | OC3M */
            t->CCMR2 &= ~0x0003UL;                  /* CC3S = 00 (output) */
        } else {
            t->CCMR2 = (t->CCMR2 & ~0xFF00UL) |
                       (0x6UL << 12) | (ocm << 12); /* OC4PE | OC4M */
            t->CCMR2 &= ~0x0300UL;                  /* CC4S = 00 (output) */
        }
    }

    /* CCER: enable output (CCxE) and set polarity (CCxP). */
    uint32_t shift = (uint32_t)(ch - 1) * 4U;
    t->CCER &= ~(0x3UL << shift);                  /* clear CCxE + CCxP */
    t->CCER |= (0x1UL << shift);                   /* CCxE = 1 (output on) */
    if (polarity) t->CCER |= (0x2UL << shift);     /* CCxP = 1 (active-low) */
}

void tim_hal_pwm_set_duty(tim_hal_handle_t *h, int ch, uint32_t duty_ticks)
{
    volatile uint32_t *ccr = pwm_ccr(h, ch);
    if (ccr) *ccr = duty_ticks;
}

void tim_hal_pwm_channel_enable(tim_hal_handle_t *h, int ch, int on)
{
    if (!h || ch < 1 || ch > 4) return;
    TIM_TypeDef *t = h->tim;
    /* Skip basic timers (TIM6/TIM7) that have no CC channels. */
    if (IS_BASIC_TIM(t)) return;
    uint32_t shift = (uint32_t)(ch - 1) * 4U;
    if (on) t->CCER |=  (0x1UL << shift);          /* CCxE = 1 */
    else    t->CCER &= ~(0x1UL << shift);          /* CCxE = 0 (float) */
}

uint32_t tim_hal_pwm_get_duty(tim_hal_handle_t *h, int ch)
{
    volatile uint32_t *ccr = pwm_ccr(h, ch);
    return ccr ? *ccr : 0U;
}

uint32_t tim_hal_pwm_period_ticks(tim_hal_handle_t *h)
{
    return h ? (h->tim->ARR + 1U) : 0U;
}

/* ===========================================================================
 * ADVANCED-TIMER features (TIM1/TIM8). See tim_hal.h for the contract.
 * Every BDTR/complementary/break accessor is a no-op on a GP TIM, so the
 * drivers can call them unconditionally.
 * =========================================================================== */

int tim_hal_is_advanced(tim_hal_handle_t *h)
{
    return (h && h->advanced) ? 1 : 0;
}

int tim_hal_set_repetition(tim_hal_handle_t *h, uint32_t rep)
{
    if (!h || !h->advanced) return -1;     /* RCR only exists on TIM1/TIM8 */
    if (rep > 255U) rep = 255U;
    h->tim->RCR = rep;
    return 0;
}

uint32_t tim_hal_get_repetition(tim_hal_handle_t *h)
{
    return (h && h->advanced) ? (h->tim->RCR & 0xFFU) : 0U;
}

void tim_hal_pwm_main_output_enable(tim_hal_handle_t *h, int on)
{
    if (!h || !h->advanced) return;        /* MOE only exists on TIM1/TIM8 */
    if (on) h->tim->BDTR |=  TIM_BDTR_MOE;
    else    h->tim->BDTR &= ~TIM_BDTR_MOE;
}

/* 4-zone DTG encoding (reference manual, TIMx_BDTR.DTG). Returns the 8-bit
 * value to write to BDTR.DTG for a dead-time of dt_ticks timer clocks. */
uint8_t tim_hal_pwm_encode_deadtime(uint32_t dt_ticks)
{
    if (dt_ticks <= 127U)        return (uint8_t)dt_ticks;                 /* zone 0 */
    if (dt_ticks <= 254U)        return (uint8_t)(0x80U | ((dt_ticks / 2U) - 64U)); /* zone 1 */
    if (dt_ticks <= 504U)        return (uint8_t)(0xC0U | ((dt_ticks / 8U) - 32U)); /* zone 2 */
    /* zone 3 (clamp to the maximum representable dead-time) */
    if (dt_ticks > 1008U) dt_ticks = 1008U;
    return (uint8_t)(0xE0U | ((dt_ticks / 16U) - 32U));
}

void tim_hal_pwm_set_deadtime(tim_hal_handle_t *h, uint8_t dtg)
{
    if (!h || !h->advanced) return;
    h->tim->BDTR = (h->tim->BDTR & ~TIM_BDTR_DTG_Msk) | (uint32_t)dtg;
}

void tim_hal_pwm_config_complementary(tim_hal_handle_t *h, int ch, int polarity_n)
{
    if (!h || !h->advanced || ch < 1 || ch > 4) return;
    TIM_TypeDef *t = h->tim;
    uint32_t shift = (uint32_t)(ch - 1) * 4U;   /* CCxNE = bit +2, CCxNP = bit +3 */
    t->CCER &= ~(0xCUL << shift);               /* clear CCxNE + CCxNP */
    t->CCER |=  (0x4UL << shift);               /* CCxNE = 1 (complementary on) */
    if (polarity_n) t->CCER |= (0x8UL << shift); /* CCxNP = 1 (active-low) */
}

void tim_hal_pwm_set_break(tim_hal_handle_t *h, int enable, int polarity)
{
    if (!h || !h->advanced) return;
    TIM_TypeDef *t = h->tim;
    t->BDTR &= ~(TIM_BDTR_BKE | TIM_BDTR_BKP);
    if (enable) {
        t->BDTR |= TIM_BDTR_BKE;
        if (polarity) t->BDTR |= TIM_BDTR_BKP;  /* 1 = break active-high */
    }
}

uint32_t tim_hal_pwm_get_bdtr(tim_hal_handle_t *h)
{
    return (h && h->advanced) ? h->tim->BDTR : 0U;
}

int tim_hal_pwm_complementary_enabled(tim_hal_handle_t *h, int ch)
{
    if (!h || !h->advanced || ch < 1 || ch > 4) return 0;
    uint32_t shift = (uint32_t)(ch - 1) * 4U;
    return (h->tim->CCER & (0x4UL << shift)) ? 1 : 0;   /* CCxNE */
}
