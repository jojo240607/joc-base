#include "wwdg_hal.h"
#include "stm32h7xx.h"
#include <stdlib.h>

/* H7 WWDG1 lives in the D3 APB4 domain (0x58002C00), NOT on APB1 like the
 * F4 — the clock gate is RCC_APB4ENR.WWDG1EN (bit 11). Register layout
 * (CR/CFR/SR) is identical to the F4. */

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
    /* Gate the WWDG1 clock on APB4. No backup-domain (DBP) dance needed and,
     * unlike the IWDG, the WWDG does NOT survive a reset. */
    RCC->APB4ENR |= RCC_APB4ENR_WWDG1EN;
}

static void wwdg_hal_write_config(wwdg_hal_handle_t *h, uint32_t wdgtb, uint32_t window)
{
    /* CFR: WDGTB[8:7] prescaler, W[6:0] window. EWI (bit 9) stays clear. */
    uint32_t cfr = ((wdgtb & 0x3U) << WWDG_CFR_WDGTB_Pos) | (window & WWDG_CFR_W_Msk);
    h->reg->CFR = cfr;
}

void wwdg_hal_set_prescaler(wwdg_hal_handle_t *h, uint32_t wdgtb)
{
    if (!h) return;
    wwdg_hal_enable(h);
    uint32_t window = h->reg->CFR & WWDG_CFR_W_Msk;   /* preserve current window */
    wwdg_hal_write_config(h, wdgtb, window);
}

void wwdg_hal_set_window(wwdg_hal_handle_t *h, uint32_t window)
{
    if (!h) return;
    wwdg_hal_enable(h);
    uint32_t wdgtb = (h->reg->CFR >> WWDG_CFR_WDGTB_Pos) & 0x3U; /* preserve prescaler */
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
    h->reg->CR = WWDG_CR_WDGA | (counter & 0x7FU);
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
