#include "rtc_hal.h"
#include "stm32h7xx.h"
#include <stdlib.h>

struct rtc_hal_handle {
    RTC_TypeDef *reg;
};

/* On this silicon a write to a calendar register (TR/DR) only reaches the
 * counter if it is flushed across the APB->RTC write-synchronisation bridge.
 * The bridge is flushed by a READ of that same register after the write. That
 * read must not be optimised away, so we store the read-back into a `volatile`
 * global — an unmistakable side effect the compiler (even under LTO) must emit.
 * NOTE: this alone is not enough — TR and DR must also each be written in their
 * OWN initialisation phase; writing both in one init phase silently drops the
 * first (DR) write. The driver enforces the per-register init phase. */
volatile uint32_t g_rtc_wb;

rtc_hal_handle_t *rtc_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    rtc_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (RTC_TypeDef *)peripheral;
    return h;
}
void rtc_hal_destroy(rtc_hal_handle_t *h) { free(h); }

/* Unlock / lock the RTC write protection (WPR). The calendar registers and the
 * INIT bit are write-protected; two magic keys disable, any other value locks. */
static void rtc_hal_unlock(rtc_hal_handle_t *h) { (void)h; RTC->WPR = 0xCAU; RTC->WPR = 0x53U; }
static void rtc_hal_lock(rtc_hal_handle_t *h)   { (void)h; RTC->WPR = 0xFFU; }

void rtc_hal_enable(rtc_hal_handle_t *h)
{
    (void)h;
    /* 1) Power on PWR (D3 APB4) and unlock the backup domain so BDCR / RTC are
     *    writable. H7: PWR clock gate is APB4ENR.PWREN and DBP lives in
     *    PWR->CR1 (the F4 used APB1ENR.PWREN + PWR->CR). */
    RCC->APB4ENR |= RCC_APB4ENR_PWREN;
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;   /* RTC APB interface clock */
    PWR->CR1 |= PWR_CR1_DBP;

    /* 2) Start LSI (internal ~32 kHz, always available — no external crystal). */
    if ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
        RCC->CSR |= RCC_CSR_LSION;
        while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) { }
    }

    /* 3) Select LSI as the RTC clock source and enable the RTC. The RTCSEL bits
     *    are write-once while RTCEN==0, so only touch them on first bring-up. */
    if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
        RCC->BDCR = (RCC->BDCR & ~RCC_BDCR_RTCSEL) | RCC_BDCR_RTCSEL_1; /* LSI */
        RCC->BDCR |= RCC_BDCR_RTCEN;
    }

    /* 4) Wait for the RTC shadow registers to be synchronised (RSF). If a prior
     *    run already set RSF this poll passes immediately — it cannot hang.
     *    H7 register name is ICSR (same offset 0x0C as the F4's ISR). */
    while ((RTC->ICSR & RTC_ICSR_RSF) == 0U) { }
}

void rtc_hal_enter_init(rtc_hal_handle_t *h)
{
    (void)h;
    rtc_hal_unlock(NULL);                 /* INIT bit + calendar regs are protected */
    RTC->ICSR |= RTC_ICSR_INIT;           /* request initialisation mode */
    while ((RTC->ICSR & RTC_ICSR_INITF) == 0U) { }
}

void rtc_hal_exit_init(rtc_hal_handle_t *h)
{
    (void)h;
    RTC->ICSR &= ~RTC_ICSR_INIT;          /* leave initialisation mode */
    RTC->ICSR &= ~RTC_ICSR_RSF;           /* force a fresh sync detection */
    while ((RTC->ICSR & RTC_ICSR_RSF) == 0U) { }
    rtc_hal_lock(NULL);
}

void rtc_hal_set_prer(rtc_hal_handle_t *h, uint32_t prediv_a, uint32_t prediv_s)
{
    if (!h) return;
    h->reg->PRER = ((prediv_a & 0x7FU) << RTC_PRER_PREDIV_A_Pos)
                 |  (prediv_s & 0x7FFFU);
}

uint32_t rtc_hal_set_tr(rtc_hal_handle_t *h, uint32_t tr)
{
    if (!h) return 0U;
    h->reg->TR = tr;
    g_rtc_wb = h->reg->TR;     /* volatile store forces the flush read */
    return h->reg->TR;
}

/* The STM32H7 RTC sits behind the same APB write-synchronization bridge as the
 * F4 (effectively 1-deep): a write to RTC_DR is NOT guaranteed to latch into
 * the calendar counter until the CPU performs a *read* of RTC_DR, which forces
 * the bridge to push the pending write across. g_rtc_wb (volatile) makes that
 * read unavoidable. Without it DR stays at its reset default (0x00002101). */
uint32_t rtc_hal_set_dr(rtc_hal_handle_t *h, uint32_t dr)
{
    if (!h) return 0U;
    h->reg->DR = dr;
    g_rtc_wb = h->reg->DR;     /* volatile store forces the flush read */
    return h->reg->DR;
}
uint32_t rtc_hal_get_tr(rtc_hal_handle_t *h) { return h ? h->reg->TR : 0U; }
uint32_t rtc_hal_get_dr(rtc_hal_handle_t *h) { return h ? h->reg->DR : 0U; }

void rtc_hal_get_prer(rtc_hal_handle_t *h, uint32_t *prediv_a, uint32_t *prediv_s)
{
    if (!h) {
        if (prediv_a) *prediv_a = 0U;
        if (prediv_s) *prediv_s = 0U;
        return;
    }
    uint32_t v = h->reg->PRER;
    if (prediv_a) *prediv_a = (v >> RTC_PRER_PREDIV_A_Pos) & 0x7FU;
    if (prediv_s)  *prediv_s = v & 0x7FFFU;
}

uint32_t rtc_hal_get_bdcr(void) { return RCC->BDCR; }
