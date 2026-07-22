#ifndef RTC_HAL_H
#define RTC_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32F4 Real-Time Clock (RTC).
 *
 * The ONLY place that touches the backup-domain and RTC registers. The driver
 * layer stays register-free and only calls these functions. The RTC is clocked
 * by the LSI internal oscillator (always available, no external crystal needed),
 * which makes the self-test deterministic and pin-independent.
 */

typedef struct rtc_hal_handle rtc_hal_handle_t;

rtc_hal_handle_t *rtc_hal_create(void *peripheral);
void rtc_hal_destroy(rtc_hal_handle_t *h);

/* Bring up the RTC clock tree: PWR clock + backup-domain unlock, start LSI,
 * select LSI as the RTC source, enable RTCEN, and wait for register sync.
 * Idempotent — safe to call on every open(). */
void rtc_hal_enable(rtc_hal_handle_t *h);

/* Enter / exit calendar initialisation mode. Writing TR / DR / PRER is only
 * allowed while in init mode, so the caller brackets those writes with these. */
void rtc_hal_enter_init(rtc_hal_handle_t *h);
void rtc_hal_exit_init(rtc_hal_handle_t *h);

/* Prescaler: async (PREDIV_A) + sync (PREDIV_S) define the 1 Hz tick. */
void rtc_hal_set_prer(rtc_hal_handle_t *h, uint32_t prediv_a, uint32_t prediv_s);

/* Raw BCD register access (the driver encodes / decodes decimal <-> BCD).
 * set_tr / set_dr RETURN the value read back immediately after the write; the
 * caller must store that return (e.g. into the live device struct) so the
 * read-back cannot be optimised away — the read is what flushes the APB->RTC
 * write-synchronization bridge and makes the write actually latch. */
uint32_t rtc_hal_set_tr(rtc_hal_handle_t *h, uint32_t tr);
uint32_t rtc_hal_set_dr(rtc_hal_handle_t *h, uint32_t dr);
uint32_t rtc_hal_get_tr(rtc_hal_handle_t *h);
uint32_t rtc_hal_get_dr(rtc_hal_handle_t *h);

/* Read-back helpers for BIST / diagnostics. */
void    rtc_hal_get_prer(rtc_hal_handle_t *h, uint32_t *prediv_a, uint32_t *prediv_s);
uint32_t rtc_hal_get_bdcr(void);   /* returns RCC->BDCR */

#endif /* RTC_HAL_H */
