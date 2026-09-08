#include "flash_hal.h"
#include "stm32h7xx.h"
#include <stdlib.h>

/*
 * STM32H7 internal FLASH HAL — milestone 1 MINIMAL set.
 *
 * The H7 dual-bank flash controller is substantially different from F4
 * (KEYR1/CR1/SR1/CCR1, 128KB sectors, option bytes, write granularity); erase
 * and program are left for milestone 2 (real-board QSPI/flash programming).
 * This file only provides the API surface so drivers link, plus the ACR
 * latency configuration used by the clock HAL and read/status helpers that
 * need no unlock.
 *
 * FLASH ACR latency is configured here AND in clock_hal_configure(); keeping
 * it here gives drivers a single place to re-apply it after a clock change.
 */

struct flash_hal_handle {
    FLASH_TypeDef *reg;
};

/* Apply the ACR setting required at 480 MHz @ VOS0: 2 wait states +
 * WRHIGHFREQ (bank clock > 110 MHz). Idempotent. */
static void flash_hal_apply_acr(void)
{
    FLASH->ACR = FLASH_ACR_LATENCY_2WS | FLASH_ACR_WRHIGHFREQ_1;
}

flash_hal_handle_t *flash_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    flash_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (FLASH_TypeDef *)peripheral;
    flash_hal_apply_acr();   /* keep ACR sane even if clock_hal ran without us */
    return h;
}
void flash_hal_destroy(flash_hal_handle_t *h) { free(h); }

/* --- milestone 2 stubs (no-op) --- */

uint32_t flash_hal_sector_base(uint32_t sector) { (void)sector; return 0U; }
uint32_t flash_hal_sector_size(uint32_t sector) { (void)sector; return 0U; }

void flash_hal_unlock(flash_hal_handle_t *h) { (void)h; }
void flash_hal_erase_sector(flash_hal_handle_t *h, uint32_t sector)
{
    (void)h; (void)sector;
}
void flash_hal_program_u32(flash_hal_handle_t *h, uint32_t addr, uint32_t value)
{
    (void)h; (void)addr; (void)value;
}

/* --- functional read/status helpers --- */

uint32_t flash_hal_read_u32(flash_hal_handle_t *h, uint32_t addr)
{
    (void)h;
    __DSB();
    return *(volatile uint32_t *)addr;
}

uint32_t flash_hal_get_status(flash_hal_handle_t *h)
{
    return h ? h->reg->SR1 : 0U;
}
