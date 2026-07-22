#ifndef RNG_HAL_H
#define RNG_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32F4 True Random Number Generator (RNG).
 *
 * The ONLY place that touches the RNG registers. The driver layer stays
 * register-free and only calls these functions. The RNG needs no external pins
 * and no clock configuration beyond gating it on the AHB2 bus, so the driver is
 * fully self-contained and pin-independent.
 */

typedef struct rng_hal_handle rng_hal_handle_t;

rng_hal_handle_t *rng_hal_create(void *peripheral);
void rng_hal_destroy(rng_hal_handle_t *h);

/* Gate the RNG clock (RCC AHB2) and enable the generator (RNG_CR.RNGEN).
 * Idempotent — safe to call on every open(). */
void rng_hal_enable(rng_hal_handle_t *h);

/* Block until data is ready (RNG_SR.DRDY) and return one 32-bit random word.
 * RNG has NO interrupt enabled, so spinning on DRDY cannot deadlock (no ISR
 * clears the flag — the hardware sets it when a new word is produced). */
uint32_t rng_hal_get_u32(rng_hal_handle_t *h);

/* Returns the raw RNG status register (RNG_SR). */
uint32_t rng_hal_get_status(rng_hal_handle_t *h);

/* Non-zero if a clock-error (CEIS) or seed-error (SEIS) is pending. */
int rng_hal_is_error(rng_hal_handle_t *h);

#endif /* RNG_HAL_H */
