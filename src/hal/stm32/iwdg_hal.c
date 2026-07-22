#include "iwdg_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

/* IWDG key-register magic values (KR is write-only). */
#define IWDG_KR_UNLOCK  0x5555U   /* permit PR/RLR writes (single unlock) */
#define IWDG_KR_RELOAD  0xAAAAU   /* reload the down-counter (feed) */
#define IWDG_KR_START   0xCCCCU   /* start the watchdog (arms the reset) */

struct iwdg_hal_handle {
    IWDG_TypeDef *reg;
};

iwdg_hal_handle_t *iwdg_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    iwdg_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (IWDG_TypeDef *)peripheral;
    return h;
}
void iwdg_hal_destroy(iwdg_hal_handle_t *h) { free(h); }

void iwdg_hal_enable(iwdg_hal_handle_t *h)
{
    (void)h;
    /* The IWDG lives in the backup domain, so its registers (KR/PR/RLR) are
     * write-protected by PWR->CR.DBP. Set it before any write; it is shared with
     * the RTC driver and may not still be set when we run. Also ensure LSI (the
     * IWDG time base) is running — idempotent (the RTC driver may have done it).
     * There is no APB clock gate for the IWDG itself. */
    PWR->CR |= PWR_CR_DBP;
    RCC->CSR |= RCC_CSR_LSION;
    while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) { }
}

void iwdg_hal_unlock(iwdg_hal_handle_t *h)
{
    if (!h) return;
    h->reg->KR = IWDG_KR_UNLOCK;
}

void iwdg_hal_set_prescaler(iwdg_hal_handle_t *h, uint32_t pr)
{
    if (!h) return;
    iwdg_hal_enable(h);
    /* Unlock then write. The PVU status bit is set by hardware on this write
     * and only clears when the counter is started/reloaded (KR=0xCCCC/0xAAAA),
     * so we must NOT spin on it here — that would hang a non-started WDT. The
     * PR configuration register itself already holds the value we wrote and is
     * readable immediately, which is all the self-test needs. */
    iwdg_hal_unlock(h);
    h->reg->PR = (uint32_t)(pr & 0x7U);
}

void iwdg_hal_set_reload(iwdg_hal_handle_t *h, uint32_t rlr)
{
    if (!h) return;
    iwdg_hal_enable(h);
    iwdg_hal_unlock(h);
    h->reg->RLR = (uint32_t)(rlr & 0x0FFFU);
}

uint32_t iwdg_hal_get_prescaler(iwdg_hal_handle_t *h)
{
    return h ? (h->reg->PR & 0x7U) : 0U;
}
uint32_t iwdg_hal_get_reload(iwdg_hal_handle_t *h)
{
    return h ? (h->reg->RLR & 0x0FFFU) : 0U;
}

void iwdg_hal_start(iwdg_hal_handle_t *h)
{
    if (!h) return;
    iwdg_hal_enable(h);
    h->reg->KR = IWDG_KR_START;
}

void iwdg_hal_refresh(iwdg_hal_handle_t *h)
{
    if (!h) return;
    h->reg->KR = IWDG_KR_RELOAD;
}

uint32_t iwdg_hal_get_status(iwdg_hal_handle_t *h)
{
    return h ? h->reg->SR : 0U;
}
