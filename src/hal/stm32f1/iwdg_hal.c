#include "iwdg_hal.h"
#include "stm32f103xx.h"
#include <stdlib.h>

#define IWDG_KR_UNLOCK  0x5555U
#define IWDG_KR_RELOAD  0xAAAAU
#define IWDG_KR_START   0xCCCCU

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