#include "rtc_hal.h"
#include "stm32f103xx.h"
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — STM32F103 RTC.
 *
 * The F103 RTC is a simple 32-bit counter driven by a configurable prescaler
 * (asynchronous divide-by PRLH/PRLL).  The counter advances once per second
 * with the usual LSI (40 kHz) / 40  = 1 kHz / 1000 = 1 Hz setup.
 *
 * Register access rules (RM0008 §16.3):
 *   1. Set CRL.CNF = 1  to enter config mode (write PRLH/PRLL/CNTH/CNTL/ALRH/ALRL).
 *   2. Write the desired registers.
 *   3. Clear CRL.CNF = 0  to latch.
 *   4. Wait for CRL.RTOFF to become 1 before the next RTC access.
 *
 * Counter reads use the shadow registers (CNTH/CNTL) — they are synchronised
 * on each APB1 read of CNTL (low byte first), which latches CNTH; then reading
 * CNTH gives the latched high half. Driver-level consumers call rtc_hal_get_cnt()
 * which reads CNTL first then CNTH.
 */

struct rtc_hal_handle {
    RTC_TypeDef *reg;
};

rtc_hal_handle_t *rtc_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    rtc_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (RTC_TypeDef *)peripheral;
    return h;
}
void rtc_hal_destroy(rtc_hal_handle_t *h) { free(h); }

/* Wait for the last RTC write to complete (RTOFF == 1). */
static void rtc_hal_wait_off(rtc_hal_handle_t *h)
{
    while (!(h->reg->CRL & RTC_CRL_RTOFF)) { }
}

void rtc_hal_enable(rtc_hal_handle_t *h)
{
    (void)h;
    /* 1) Power on PWR and unlock the backup domain for BDCR writes. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;

    /* 2) Start LSI (~40 kHz). */
    if ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
        RCC->CSR |= RCC_CSR_LSION;
        while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) { }
    }

    /* 3) Select LSI as RTC clock, enable RTC. */
    if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
        RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_LSI;
        RCC->BDCR |= RCC_BDCR_RTCEN;
    }
}

void rtc_hal_set_prescaler(rtc_hal_handle_t *h, uint32_t prl)
{
    if (!h) return;
    h->reg->CRL |= RTC_CRL_CNF;          /* enter config mode */
    rtc_hal_wait_off(h);
    h->reg->PRLH = (uint16_t)((prl >> 16) & 0xFFFFU);
    h->reg->PRLL = (uint16_t)(prl & 0xFFFFU);
    h->reg->CRL &= ~RTC_CRL_CNF;         /* leave config mode */
    rtc_hal_wait_off(h);
}

void rtc_hal_set_cnt(rtc_hal_handle_t *h, uint32_t cnt)
{
    if (!h) return;
    h->reg->CRL |= RTC_CRL_CNF;          /* enter config mode */
    rtc_hal_wait_off(h);
    h->reg->CNTH = (uint16_t)((cnt >> 16) & 0xFFFFU);
    h->reg->CNTL = (uint16_t)(cnt & 0xFFFFU);
    h->reg->CRL &= ~RTC_CRL_CNF;         /* leave config mode */
    rtc_hal_wait_off(h);
}

uint32_t rtc_hal_get_cnt(rtc_hal_handle_t *h)
{
    if (!h) return 0U;
    /* Read CNTL first to latch CNTH shadow, then read CNTH. */
    uint16_t lo = (uint16_t)(h->reg->CNTL & 0xFFFFU);
    uint16_t hi = (uint16_t)(h->reg->CNTH & 0xFFFFU);
    return ((uint32_t)hi << 16) | lo;
}

void rtc_hal_set_alarm(rtc_hal_handle_t *h, uint32_t alarm)
{
    if (!h) return;
    h->reg->CRL |= RTC_CRL_CNF;          /* enter config mode */
    rtc_hal_wait_off(h);
    h->reg->ALRH = (uint16_t)((alarm >> 16) & 0xFFFFU);
    h->reg->ALRL = (uint16_t)(alarm & 0xFFFFU);
    h->reg->CRL &= ~RTC_CRL_CNF;         /* leave config mode */
    rtc_hal_wait_off(h);
}

void rtc_hal_enable_alarm_irq(rtc_hal_handle_t *h)
{
    if (!h) return;
    h->reg->CRH |= RTC_CRH_ALRIE;
}

void rtc_hal_disable_alarm_irq(rtc_hal_handle_t *h)
{
    if (!h) return;
    h->reg->CRH &= ~RTC_CRH_ALRIE;
}

void rtc_hal_clear_alarm_flag(rtc_hal_handle_t *h)
{
    if (!h) return;
    h->reg->CRL &= ~RTC_CRL_ALRF;
}

uint32_t rtc_hal_get_crl(rtc_hal_handle_t *h)
{
    return h ? h->reg->CRL : 0U;
}

uint32_t rtc_hal_get_crh(rtc_hal_handle_t *h)
{
    return h ? h->reg->CRH : 0U;
}

uint32_t rtc_hal_get_bdcr(void)
{
    return RCC->BDCR;
}