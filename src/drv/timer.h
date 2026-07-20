#ifndef TIMER_H
#define TIMER_H

#include "iface/event_device.h"   /* event_device base class (this driver IS-A event_device) */
#include "tim_hal.h"              /* opaque HAL handle (driver never sees TIM_TypeDef) */
#include <stdint.h>

/*
 * General-purpose TIMER driver — an EVENT device (periodic timer alarm).
 *
 * A GP TIM is modeled as a periodic EVENT source, exactly like SysTick: it
 * configures the timer (via the STM32 HAL) and registers its overflow ISR
 * through the PLATFORM-INDEPENDENT irq framework. Subscribers get a
 * DEVICE_EVENT_TICK notification on every update (overflow) event.
 *
 * enable() starts the counter AND arms the NVIC; disable() stops the counter
 * and masks the NVIC. The overflow ISR MUST clear UIF or the interrupt
 * re-enters forever.
 */
typedef struct _timer timer;

/* Board fills this as DATA. timer_clk_hz is the clock feeding the timer
 * (TIM2..TIM5 on F4 = 84 MHz APB1 timer clock), tick_hz the desired rate. */
typedef struct {
    const char *name;        /* logical device name (e.g. "timer0") */
    void *peripheral;        /* TIM2..TIM5 base (board supplies the real silicon) */
    uint32_t timer_clk_hz;   /* clock feeding this timer */
    uint32_t tick_hz;        /* desired overflow rate (Hz) */
} timer_config_t;

struct _timer {
    event_device parent;        /* IS-A event_device IS-A device */
    tim_hal_handle_t *hal;      /* opaque HAL handle */
    irq_id_t irq;               /* this timer's interrupt id (from HAL) */
    uint32_t timer_clk_hz;      /* cached from config (used at open) */
    uint32_t tick_hz;           /* cached from config (used at open) */
    uint32_t repetition;        /* advanced-TIM RCR: TICK every (rep+1) overflows */
    volatile uint32_t overflows;/* free-running overflow counter (ISR increments) */
    device_event_cb_t cb;       /* subscribed tick callback (or NULL) */
    void *cb_ctx;               /* callback context */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *timer_create(const void *config);
void timer_destroy(timer *self);

/* ioctl commands (driver-specific) */
#define TIMER_IOCTL_GET_OVERFLOWS  0x01   /* arg = uint32_t* : overflow count */
#define TIMER_IOCTL_GET_COUNTER    0x02   /* arg = uint32_t* : current CNT */
#define TIMER_IOCTL_SET_REPETITION 0x03   /* arg = uint32_t* : RCR 0..255 (adv TIM) */
#define TIMER_IOCTL_GET_REPETITION 0x04   /* arg = uint32_t* : current RCR */

#endif /* TIMER_H */
