#ifndef WWDG_HAL_H
#define WWDG_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32F4 Window Watchdog (WWDG).
 *
 * The ONLY place that touches the WWDG registers. The driver stays
 * register-free. Unlike the IWDG, the WWDG lives in the APB1 domain (needs the
 * WWDGEN clock gate) and its configuration register (CFR) is directly readable,
 * so a program / readback round-trip is possible WITHOUT activating the counter.
 * The self-test deliberately does NOT set WDGA (activate) — that would start the
 * down-counter and reset the board if not fed. WWDG is NOT in the backup domain,
 * so even an accidental activation only affects the current boot (it clears on
 * reset), unlike the IWDG which would brick the board.
 */

typedef struct wwdg_hal_handle wwdg_hal_handle_t;

wwdg_hal_handle_t *wwdg_hal_create(void *peripheral);
void wwdg_hal_destroy(wwdg_hal_handle_t *h);

/* Gate the WWDG clock (RCC APB1). Idempotent. */
void wwdg_hal_enable(wwdg_hal_handle_t *h);

/* Program prescaler (WDGTB code 0..3) and window (0x40..0x7F) into CFR. Each
 * preserves the other field (read-modify-write). EWI (early-wakeup) stays 0. */
void wwdg_hal_set_prescaler(wwdg_hal_handle_t *h, uint32_t wdgtb);
void wwdg_hal_set_window(wwdg_hal_handle_t *h, uint32_t window);

/* Read back the configuration register (CFR). */
uint32_t wwdg_hal_get_config(wwdg_hal_handle_t *h);

/* Arm the watchdog: set WDGA. The down-counter runs and a reset occurs unless a
 * refresh is performed in the valid window. ONLY call when feeding. */
void wwdg_hal_start(wwdg_hal_handle_t *h, uint32_t counter);

/* Reload the counter (write CR.T without WDGA). `counter` must be > window and
 * <= 0x7F, else the refresh itself triggers a reset. */
void wwdg_hal_refresh(wwdg_hal_handle_t *h, uint32_t counter);

/* Raw counter (CR.T) and status (SR) registers. */
uint32_t wwdg_hal_get_counter(wwdg_hal_handle_t *h);
uint32_t wwdg_hal_get_status(wwdg_hal_handle_t *h);

#endif /* WWDG_HAL_H */
