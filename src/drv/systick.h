#ifndef SYSTICK_H
#define SYSTICK_H

#include "iface/event_device.h"   /* event_device base class (this driver IS-A event_device) */
#include <stdint.h>

/*
 * SysTick driver — a textbook EVENT device.
 *
 * SysTick is the Cortex-M core timer. Through the four-class model it is a
 * periodic-timer EVENT source (a "timer alarm / RTC wakeup" sibling), NOT a
 * stream/block/control device. So it is implemented as an `event_device`:
 *   - the driver configures the core timer (Cortex-M generic, via CMSIS
 *     SysTick_Config) and registers its ISR through the PLATFORM-INDEPENDENT
 *     irq framework (irq_register). No vector-table / NVIC code in the driver.
 *   - the application subscribes with set_event_callback(DEVICE_EVENT_TICK, cb)
 *     and gets a tick notification on every overflow.
 *
 * This proves, in one driver: (a) the four-class split is feasible, and (b)
 * every event device naturally rides the unified irq framework.
 */
typedef struct _systick systick;

/* Board fills this as DATA. cpu_hz is the core clock the board knows; tick_hz
 * is the desired tick rate (e.g. 1000 = 1 ms tick). */
typedef struct {
    const char *name;       /* logical device name (e.g. "systick") */
    uint32_t cpu_hz;        /* core clock frequency, supplied by the board */
    uint32_t tick_hz;       /* desired tick frequency */
} systick_config_t;

struct _systick {
    event_device parent;        /* IS-A event_device IS-A device */
    volatile uint32_t ticks;    /* free-running tick counter (ISR increments) */
    device_event_cb_t cb;       /* subscribed tick callback (or NULL) */
    void *cb_ctx;               /* callback context */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *systick_create(const void *config);
void systick_destroy(systick *self);

#endif /* SYSTICK_H */
