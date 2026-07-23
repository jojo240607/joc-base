#include "wwdg_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

struct wwdg_hal_handle {
    WWDG_TypeDef *reg;
};

wwdg_hal_handle_t *wwdg_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    wwdg_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (WWDG_TypeDef *)peripheral;
    return h;
}
void wwdg_hal_destroy(wwdg_hal_handle_t *h) { free(h); }

void wwdg_hal_enable(wwdg_hal_handle_t *h)
{
    (void)h;
    /* The WWDG is on APB1; gate its clock. No backup-domain (DBP) dance needed
     * and, unlike the IWDG, the WWDG does NOT survive a reset. */
    RCC->APB1ENR |= RCC_APB1ENR_WWDGEN;
}

static void wwdg_hal_write_config(wwdg_hal_handle_t *h, uint32_t wdgtb, uint32_t window)
{
    /* CFR: WDGTB[8:7] prescaler, W[6:0] window. EWI (bit 9) stays clear. */
    uint32_t cfr = ((wdgtb & 0x3U) << 7) | (window & 0x7FU);
    h->reg->CFR = cfr;
}

void wwdg_hal_set_prescaler(wwdg_hal_handle_t *h, uint32_t wdgtb)
{
    if (!h) return;
    wwdg_hal_enable(h);
    uint32_t window = h->reg->CFR & 0x7FU;        /* preserve current window */
    wwdg_hal_write_config(h, wdgtb, window);
}

void wwdg_hal_set_window(wwdg_hal_handle_t *h, uint32_t window)
{
    if (!h) return;
    wwdg_hal_enable(h);
    uint32_t wdgtb = (h->reg->CFR >> 7) & 0x3U;   /* preserve current prescaler */
    wwdg_hal_write_config(h, wdgtb, window);
}

uint32_t wwdg_hal_get_config(wwdg_hal_handle_t *h)
{
    return h ? h->reg->CFR : 0U;
}

void wwdg_hal_start(wwdg_hal_handle_t *h, uint32_t counter)
{
    if (!h) return;
    wwdg_hal_enable(h);
    /* WDGA (bit 7) activates; T[6:0] loads the counter. */
    h->reg->CR = (0x1U << 7) | (counter & 0x7FU);
}

void wwdg_hal_refresh(wwdg_hal_handle_t *h, uint32_t counter)
{
    if (!h) return;
    /* Write CR.T without WDGA: reloads the counter (does not activate). */
    h->reg->CR = counter & 0x7FU;
}

uint32_t wwdg_hal_get_counter(wwdg_hal_handle_t *h)
{
    return h ? (h->reg->CR & 0x7FU) : 0U;
}

uint32_t wwdg_hal_get_status(wwdg_hal_handle_t *h)
{
    return h ? h->reg->SR : 0U;
}
