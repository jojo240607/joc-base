#ifndef TIM_HAL_H
#define TIM_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/*
 * Hardware Abstraction Layer — General-Purpose TIMER (STM32 implementation).
 *
 * Same layering contract as adc_hal.h: the driver (drv/timer.c) includes THIS
 * header but NEVER sees TIM_TypeDef or any chip register; it only ever handles
 * the OPAQUE `tim_hal_handle_t *`. All register knowledge lives in tim_hal.c.
 *
 * A general-purpose TIM is used here as a periodic overflow EVENT source, so
 * the driver is an `event_device`: it arms the update interrupt (UIE) and
 * registers its ISR through the platform-independent irq framework. The board
 * supplies the real TIM base (TIM2..TIM5) and the clock feeding it.
 */
typedef struct tim_hal_handle tim_hal_handle_t;

/* platform-specific construction: the board passes the real peripheral (cast to
 * void*); everything else is hidden inside the handle. */
tim_hal_handle_t *tim_hal_create(void *peripheral);
void tim_hal_destroy(tim_hal_handle_t *h);

void tim_hal_enable_clock(tim_hal_handle_t *h);

/* Compute PSC/ARR for the requested overflow rate. timer_clk_hz is the clock
 * feeding this timer (board knows it: TIM2..TIM5 on F4 run from the 84 MHz
 * APB1 timer clock); tick_hz is the desired overflow frequency. */
void tim_hal_config(tim_hal_handle_t *h, uint32_t timer_clk_hz, uint32_t tick_hz);

void tim_hal_start(tim_hal_handle_t *h);    /* set CR1.CEN (begin counting) */
void tim_hal_stop(tim_hal_handle_t *h);     /* clear CR1.CEN (freeze) */
uint32_t tim_hal_get_counter(tim_hal_handle_t *h);

/* Route this timer's UPDATE event to its TRGO output (CR2.MMS = 0b010). The DAC
 * (or ADC) trigger logic samples TRGO to initiate a conversion / raise a DMA
 * request, so this is how a timer clocks a DMA burst with no CPU or ISR
 * involvement. No interrupt is enabled — TRGO is a pure hardware signal. */
void tim_hal_master_trgo_update(tim_hal_handle_t *h);

/* --- interrupt support (used by the driver via the platform-independent
 *     irq framework: it registers tim_hal_irq_id() with irq_manager) --- */
irq_id_t tim_hal_irq_id(tim_hal_handle_t *h);          /* chip IRQn for this TIM */
void tim_hal_enable_update_irq(tim_hal_handle_t *h);   /* set TIM_DIER_UIE */
void tim_hal_disable_update_irq(tim_hal_handle_t *h);  /* clear TIM_DIER_UIE */
void tim_hal_clear_uif(tim_hal_handle_t *h);

/* Returns non-zero if this timer's Update Interrupt Flag (UIF) is set. Drivers
 * that share an IRQ line MUST check this before acting, so a sibling peripheral
 * on the same line doesn't spuriously trigger their handler. */
int tim_hal_uif_pending(tim_hal_handle_t *h);           /* clear TIM_SR_UIF */

/* --- TIMER DMA (Update-event burst) ---
 * A TIM's Update (overflow) event is itself a DMA request source. Enabling it
 * (DIER.UDE) lets a DMA stream move a word into any CCR on every overflow — e.g.
 * a CPU-less PWM duty sweep, or feeding a DAC/ADC trigger. The driver supplies
 * the stream (resolved from dma_hal_route) and the destination CCR address. */
void tim_hal_dma_update_enable(tim_hal_handle_t *h);    /* set DIER.UDE (UP DMA req) */
void tim_hal_dma_update_disable(tim_hal_handle_t *h);   /* clear DIER.UDE */
volatile uint32_t *tim_hal_get_ccr(tim_hal_handle_t *h, int ch); /* &TIMx_CCRx (PAR) */
uint32_t tim_hal_get_arr(tim_hal_handle_t *h);          /* ARR (bound CCR values) */

/* ===========================================================================
 * PWM CHANNEL support — the SAME TIM peripheral, used as a waveform generator.
 *
 * A GP TIM is BOTH an overflow EVENT source (tim_hal_config, used by drv/timer)
 * AND a multi-channel PWM generator (below). The two facets share one silicon
 * state (PSC/ARR/CNT), so a single TIM can drive BOTH a periodic TICK (timer
 * driver) AND PWM outputs (pwm driver) at once — that is the "coordination"
 * between the two drivers. To keep ownership clean:
 *   - the PERIOD (PSC/ARR) and the counter start/stop are owned by whichever
 *     driver configured them (normally the timer driver in "coordinate" mode);
 *   - the PWM driver only touches CCMR/CCER/CCR (channel mode, polarity, duty)
 *     and the output pin AF. It never reprograms PSC/ARR or stops the counter.
 * All register knowledge stays here, so drv/pwm never sees TIM_TypeDef.
 * =========================================================================== */

/* Compute PSC/ARR for a target PWM frequency and program them (up-count, edge
 * aligned). Returns the period in timer ticks (ARR+1), or 0 on bad args. Safe to
 * call while the counter is already running (e.g. a sibling timer driver owns
 * it) — it only reloads PSC/ARR. In "coordinate" mode the pwm driver SKIPS this
 * and reuses the timer driver's period instead. */
uint32_t tim_hal_pwm_set_period(tim_hal_handle_t *h, uint32_t timer_clk_hz,
                                uint32_t freq_hz);

/* Configure one channel (1..4) as a PWM output. mode: 1 = PWM mode 1,
 * 2 = PWM mode 2 (output high while CNT < CCR, vs the inverse). polarity:
 * 0 = active-high, 1 = active-low. Enables preload (OCxPE) and the channel
 * output (CCER.CCxE). Does NOT set the duty — call tim_hal_pwm_set_duty. */
void tim_hal_pwm_config_channel(tim_hal_handle_t *h, int ch, int mode, int polarity);

/* Set the duty as a raw compare value in timer ticks (0 .. period_ticks). */
void tim_hal_pwm_set_duty(tim_hal_handle_t *h, int ch, uint32_t duty_ticks);

/* Enable/disable ONLY the channel output (CCER.CCxE), leaving the counter and
 * the other channels untouched. on=0 also leaves the pin floating-safe. */
void tim_hal_pwm_channel_enable(tim_hal_handle_t *h, int ch, int on);

/* Readback helpers (used by the driver/selftest to verify register state). */
uint32_t tim_hal_pwm_get_duty(tim_hal_handle_t *h, int ch);  /* current CCRx */
uint32_t tim_hal_pwm_period_ticks(tim_hal_handle_t *h);      /* ARR + 1 */

/* ===========================================================================
 * ADVANCED-TIMER features (TIM1 / TIM8 on F4).
 *
 * The advanced TIMs add silicon the general-purpose TIMs lack:
 *   - a REPETITION COUNTER (RCR): the Update event (and thus the timer
 *     driver's TICK) only fires every (RCR+1) counter overflows — a clean way
 *     to divide the TICK rate without touching PSC/ARR;
 *   - a BDTR register with the Main Output Enable (MOE), a Dead-Time Generator
 *     (DTG) and a Break (BRK) fault input. On TIM1/TIM8 the PWM pins stay
 *     INACTIVE until MOE=1, so a PWM driver that ignores BDTR produces NO
 *     output on an advanced TIM — that is the gap these helpers close.
 * All of these are NO-OPS (or return -1 / 0) on a general-purpose TIM, so the
 * drivers can call them unconditionally and stay chip-agnostic.
 * =========================================================================== */

/* Non-zero iff this handle is an advanced TIM (TIM1/TIM8). Drivers use it to
 * decide whether to touch BDTR / the complementary outputs. */
int tim_hal_is_advanced(tim_hal_handle_t *h);

/* Repetition counter (RCR). rep is 0..255; the Update event fires every
 * (rep+1) overflows. Advanced TIMs only — returns -1 on a GP TIM (where RCR
 * does not exist). Read back with tim_hal_get_repetition. */
int  tim_hal_set_repetition(tim_hal_handle_t *h, uint32_t rep);
uint32_t tim_hal_get_repetition(tim_hal_handle_t *h);   /* current RCR (0 on GP) */

/* Main Output Enable (BDTR.MOE). Must be 1 for any PWM pin to drive on an
 * advanced TIM. No-op on a GP TIM. */
void tim_hal_pwm_main_output_enable(tim_hal_handle_t *h, int on);

/* Encode a dead-time in timer ticks into the 8-bit DTG field (4-zone formula
 * from the F4 reference manual). Returns the DTG byte to write to BDTR.DTG. */
uint8_t tim_hal_pwm_encode_deadtime(uint32_t dt_ticks);
/* Program the dead-time (BDTR.DTG). Advanced TIMs only. */
void tim_hal_pwm_set_deadtime(tim_hal_handle_t *h, uint8_t dtg);

/* Enable the COMPLEMENTARY output (CHxN) for a channel and set its polarity
 * (0 = active-high, 1 = active-low). The channel itself must already be in a
 * PWM mode (tim_hal_pwm_config_channel). Advanced TIMs only. */
void tim_hal_pwm_config_complementary(tim_hal_handle_t *h, int ch, int polarity_n);

/* Configure the Break (fault) input: enable != 0 arms BRK; polarity 0 = the
 * break is active-low on BRK, 1 = active-high. On a break the hardware clears
 * MOE automatically (default action). Advanced TIMs only. */
void tim_hal_pwm_set_break(tim_hal_handle_t *h, int enable, int polarity);

/* Readback helpers for verification (return 0 on a GP TIM). */
uint32_t tim_hal_pwm_get_bdtr(tim_hal_handle_t *h);            /* raw BDTR */
int      tim_hal_pwm_complementary_enabled(tim_hal_handle_t *h, int ch); /* CCER.CCxNE */

#endif /* TIM_HAL_H */
