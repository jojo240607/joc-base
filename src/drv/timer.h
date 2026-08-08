#ifndef TIMER_H
#define TIMER_H

#include "iface/event_device.h"   /* event_device base class (this driver IS-A event_device) */
#include "tim_hal.h"              /* opaque HAL handle (driver never sees TIM_TypeDef) */
#include "dma_hal.h"              /* dma_req_id_t — TIM update-event DMA request */
#include "drv/dma.h"              /* dma / dma_stream_t — TIM DMA burst state */
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
    /* TIM update-event DMA request (DMA_REQ_NONE = no DMA; the timer then runs
     * as a plain periodic event source). When set, the driver reserves the
     * fixed DMA stream for this TIM's UP event at open() so timer_dma_burst()
     * can stream data into a CCR on every overflow (CPU-less PWM duty sweep). */
    dma_req_id_t dma_req;
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
    /* DMA state (reserved at open when dma_req != DMA_REQ_NONE). NULL otherwise. */
    dma_req_id_t  dma_req;      /* cached from config */
    dma          *dma_dev;      /* owning controller ("dma1"/"dma2") or NULL */
    dma_stream_t *dma_str;      /* reserved stream for the UP-event DMA or NULL */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *timer_create(const void *config);
void timer_destroy(timer *self);

/* TIMER DMA burst: on each Update (overflow) event, the reserved DMA stream
 * moves the next 16-bit value from `buf` into TIMx_CCRx (ch = 1..4). This drives
 * a CPU-less PWM duty sweep (or any CCR-fed peripheral). `buf` may live in CCM /
 * Flash — it is copied into a main-SRAM staging buffer first (the DMA master
 * cannot reach CCM). Returns 0 on success, -1 if DMA is unavailable or args bad.
 * Requires the timer to be counting (event_device enable()) so overflows occur. */
int timer_dma_burst(timer *t, int ch, const uint16_t *buf, uint16_t n);

/* Read back the current compare register value (TIMx_CCRx). Useful to verify a
 * DMA burst landed (after timer_dma_burst the last value must be in CCRx). */
uint32_t timer_get_ccr(timer *t, int ch);

/* ioctl commands (driver-specific) */
#define TIMER_IOCTL_GET_OVERFLOWS  0x01   /* arg = uint32_t* : overflow count */
#define TIMER_IOCTL_GET_COUNTER    0x02   /* arg = uint32_t* : current CNT */
#define TIMER_IOCTL_SET_REPETITION 0x03   /* arg = uint32_t* : RCR 0..255 (adv TIM) */
#define TIMER_IOCTL_GET_REPETITION 0x04   /* arg = uint32_t* : current RCR */
/* ENABLE/DISABLE start/stop the counter + arm/mask the NVIC update IRQ. These
 * let an application drive the timer through the plain dev_* interface (open +
 * ioctl) without needing the event_device vtable's enable() method — required
 * by the App service-table (app_slot_t) path where only dev_open/dev_ioctl are
 * exported. The driver's ISR clears UIF; sibling callbacks on the same IRQ line
 * (e.g. an App-registered att_isr_give) just observe the tick. */
#define TIMER_IOCTL_ENABLE         0x05   /* arg = NULL : start counting + arm IRQ */
#define TIMER_IOCTL_DISABLE        0x06   /* arg = NULL : stop counting + mask IRQ */

#endif /* TIMER_H */
