#ifndef OSAL_H
#define OSAL_H

#include <stddef.h>

/*
 * OS Abstraction Layer (OSAL)
 * ----------------------------
 * A deliberately tiny synchronization layer so the driver framework is
 * RTOS-agnostic. Today it is backed by a bare-metal implementation
 * (osal_baremetal.c): a semaphore is a `volatile int` counter guarded by
 * PRIMASK, and osal_sem_wait() BUSY-WAITS with interrupts ENABLED so the ISR
 * that gives the semaphore can still run.
 *
 * To port to a real RTOS (FreeRTOS / a self-developed kernel), replace
 * osal_baremetal.c with an implementation that maps these three calls onto the
 * kernel's semaphore + critical-section primitives. NO driver or framework
 * code above this file needs to change — that is the entire point of the
 * abstraction.
 */

typedef struct osal_sem {
    volatile int count;   /* 0 = taken/empty, >0 = available permits */
} osal_sem_t;

/* initialize a semaphore with `val` permits (normally 0 = a completion flag) */
void osal_sem_init(osal_sem_t *s, int val);

/* take one permit, blocking (busy-wait on bare metal) until one is available.
 * MUST be called from thread context with interrupts ENABLED. */
void osal_sem_wait(osal_sem_t *s);

/* give one permit. Safe to call from interrupt context. */
void osal_sem_give(osal_sem_t *s);

/* critical-section guards (thin wrappers over the port's IRQ masking).
 * enter returns a token that exit hands back; if IRQs were already off on
 * entry, exit leaves them off (it never re-enables a pre-existing lockout). */
unsigned osal_enter_critical(void);
void     osal_exit_critical(unsigned state);

#endif /* OSAL_H */
