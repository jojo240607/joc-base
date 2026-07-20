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

/* --- interrupt support (used by the driver via the platform-independent
 *     irq framework: it registers tim_hal_irq_id() with irq_manager) --- */
irq_id_t tim_hal_irq_id(tim_hal_handle_t *h);          /* chip IRQn for this TIM */
void tim_hal_enable_update_irq(tim_hal_handle_t *h);   /* set TIM_DIER_UIE */
void tim_hal_disable_update_irq(tim_hal_handle_t *h);  /* clear TIM_DIER_UIE */
void tim_hal_clear_uif(tim_hal_handle_t *h);           /* clear TIM_SR_UIF */

#endif /* TIM_HAL_H */
