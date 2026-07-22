#ifndef IWDG_HAL_H
#define IWDG_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32F4 Independent Watchdog (IWDG).
 *
 * The ONLY place that touches the IWDG registers. The driver layer stays
 * register-free and only calls these functions. The IWDG is clocked by the
 * internal LSI oscillator — no external pins, no APB clock gate — and once
 * started (KR=0xCCCC) it can only be stopped by a reset. The self-test
 * deliberately does NOT start the counter, so it never resets the board; it
 * only exercises the unlock -> program -> status-settle -> readback path,
 * which proves the HAL drives the real silicon correctly.
 */

typedef struct iwdg_hal_handle iwdg_hal_handle_t;

iwdg_hal_handle_t *iwdg_hal_create(void *peripheral);
void iwdg_hal_destroy(iwdg_hal_handle_t *h);

/* Make sure the LSI clock (the IWDG time base) is running. Idempotent. */
void iwdg_hal_enable(iwdg_hal_handle_t *h);

/* Unlock PR/RLR for writing (write KR=0x5555). Required before each program. */
void iwdg_hal_unlock(iwdg_hal_handle_t *h);

/* Program prescaler (code 0..7) / reload (0..4095). Each performs its own
 * unlock and waits for the hardware transfer to finish (SR PVU/RVU clear). */
void iwdg_hal_set_prescaler(iwdg_hal_handle_t *h, uint32_t pr);
void iwdg_hal_set_reload(iwdg_hal_handle_t *h, uint32_t rlr);

/* Read back the currently programmed values (valid after the transfer settles). */
uint32_t iwdg_hal_get_prescaler(iwdg_hal_handle_t *h);
uint32_t iwdg_hal_get_reload(iwdg_hal_handle_t *h);

/* Arm the watchdog: the down-counter runs and a reset occurs if it is not
 * refreshed before reaching zero. ONLY call this when you intend to keep
 * feeding it (e.g. via a periodic systick callback). */
void iwdg_hal_start(iwdg_hal_handle_t *h);

/* Feed / reload the counter (write KR=0xAAAA). */
void iwdg_hal_refresh(iwdg_hal_handle_t *h);

/* Raw IWDG status register (SR). */
uint32_t iwdg_hal_get_status(iwdg_hal_handle_t *h);

#endif /* IWDG_HAL_H */
