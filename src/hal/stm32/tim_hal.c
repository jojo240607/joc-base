#include "tim_hal.h"
#include <stdlib.h>
#include <string.h>
#include <stm32f4xx.h>     /* TIM_TypeDef, RCC, IRQn_Type — board/chip layer only */

/*
 * STM32 general-purpose TIMER HAL.
 *
 * The concrete handle holds only the TIM_TypeDef*; the driver never touches it.
 * The driver asks this HAL to: enable the peripheral clock, compute PSC/ARR for
 * a target overflow rate, start/stop counting, and manage the update interrupt.
 */

struct tim_hal_handle {
    TIM_TypeDef *tim;
};

tim_hal_handle_t *tim_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    tim_hal_handle_t *h = (tim_hal_handle_t *)malloc(sizeof(tim_hal_handle_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(*h));
    h->tim = (TIM_TypeDef *)peripheral;
    return h;
}

void tim_hal_destroy(tim_hal_handle_t *h)
{
    if (!h) return;
    h->tim->CR1 &= ~TIM_CR1_CEN;   /* stop counting before freeing */
    free(h);
}

/* APB1 timers: TIM2..TIM7 and TIM12..TIM14 (timer clock = 2xAPB1 = 84 MHz on
 * this board). APB2 timers: TIM1, TIM8..TIM11 (timer clock = 2xAPB2 = 168 MHz).
 * Enable the matching RCC clock bit for each. (TIM10/TIM13 are intentionally
 * mapped here too, but are NOT instantiated on the board yet — they share an
 * IRQ line with TIM1_UP / TIM8_UP, which the single-handler-per-IRQ irq_manager
 * can't host simultaneously. That shared-line problem is deferred.) */
static void tim_hal_clock_on(TIM_TypeDef *t)
{
    if (t == TIM2)      RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    else if (t == TIM3) RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    else if (t == TIM4) RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;
    else if (t == TIM5) RCC->APB1ENR |= RCC_APB1ENR_TIM5EN;
    else if (t == TIM6) RCC->APB1ENR |= RCC_APB1ENR_TIM6EN;
    else if (t == TIM7) RCC->APB1ENR |= RCC_APB1ENR_TIM7EN;
    else if (t == TIM12) RCC->APB1ENR |= RCC_APB1ENR_TIM12EN;
    else if (t == TIM13) RCC->APB1ENR |= RCC_APB1ENR_TIM13EN;
    else if (t == TIM14) RCC->APB1ENR |= RCC_APB1ENR_TIM14EN;
    else if (t == TIM1)  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
    else if (t == TIM8)  RCC->APB2ENR |= RCC_APB2ENR_TIM8EN;
    else if (t == TIM9)  RCC->APB2ENR |= RCC_APB2ENR_TIM9EN;
    else if (t == TIM10) RCC->APB2ENR |= RCC_APB2ENR_TIM10EN;
    else if (t == TIM11) RCC->APB2ENR |= RCC_APB2ENR_TIM11EN;
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

    t->CR1 = 0;                  /* stop, reset control defaults */
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

uint32_t tim_hal_get_counter(tim_hal_handle_t *h)
{
    return h ? h->tim->CNT : 0U;
}

irq_id_t tim_hal_irq_id(tim_hal_handle_t *h)
{
    if (!h) return -1;
    TIM_TypeDef *t = h->tim;
    /* We only ever arm the UPDATE interrupt (DIER UIE), so each timer maps to
     * the IRQ line its update/global interrupt lives on:
     *   TIM1_UP/TIM10 share 25, TIM8_UP/TIM13 share 44, TIM1_BRK/TIM9 share 24,
     *   TIM1_TRG_COM/TIM11 share 26, TIM8_BRK/TIM12 share 43, TIM8_TRG_COM/TIM14
     *   share 45. The board only instantiates the lines that do NOT collide
     *   (TIM1, TIM8, TIM9, TIM11, TIM12, TIM14, TIM6, TIM7); TIM10/TIM13 are
     *   deferred until the irq_manager can host two handlers on one line. */
    if (t == TIM1)  return (irq_id_t)TIM1_UP_TIM10_IRQn;
    if (t == TIM8)  return (irq_id_t)TIM8_UP_TIM13_IRQn;
    if (t == TIM9)  return (irq_id_t)TIM1_BRK_TIM9_IRQn;
    if (t == TIM10) return (irq_id_t)TIM1_UP_TIM10_IRQn;
    if (t == TIM11) return (irq_id_t)TIM1_TRG_COM_TIM11_IRQn;
    if (t == TIM12) return (irq_id_t)TIM8_BRK_TIM12_IRQn;
    if (t == TIM13) return (irq_id_t)TIM8_UP_TIM13_IRQn;
    if (t == TIM14) return (irq_id_t)TIM8_TRG_COM_TIM14_IRQn;
    if (t == TIM2)  return (irq_id_t)TIM2_IRQn;
    if (t == TIM3)  return (irq_id_t)TIM3_IRQn;
    if (t == TIM4)  return (irq_id_t)TIM4_IRQn;
    if (t == TIM5)  return (irq_id_t)TIM5_IRQn;
    if (t == TIM6)  return (irq_id_t)TIM6_DAC_IRQn;
    if (t == TIM7)  return (irq_id_t)TIM7_IRQn;
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
    if (h) h->tim->SR &= ~TIM_SR_UIF;
}

int tim_hal_uif_pending(tim_hal_handle_t *h)
{
    return (h && (h->tim->SR & TIM_SR_UIF)) ? 1 : 0;
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

void tim_hal_pwm_config_channel(tim_hal_handle_t *h, int ch, int mode, int polarity)
{
    if (!h || ch < 1 || ch > 4) return;
    TIM_TypeDef *t = h->tim;
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
