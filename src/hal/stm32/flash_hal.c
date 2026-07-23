#include "flash_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

/* FLASH key-register unlock magic values (write-only). */
#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU

/* STM32F407 (1 MB) sector base addresses. */
static const uint32_t flash_sector_base_tbl[12] = {
    0x08000000U, 0x08004000U, 0x08008000U, 0x0800C000U,
    0x08010000U, 0x08020000U, 0x08040000U, 0x08060000U,
    0x08080000U, 0x080A0000U, 0x080C0000U, 0x080E0000U
};

/* STM32F407 (1 MB) sector sizes in bytes: 16K/16K/16K/16K/64K then 7x128K. */
static const uint32_t flash_sector_size_tbl[12] = {
    0x00004000U, 0x00004000U, 0x00004000U, 0x00004000U,
    0x00010000U, 0x00020000U, 0x00020000U, 0x00020000U,
    0x00020000U, 0x00020000U, 0x00020000U, 0x00020000U
};

struct flash_hal_handle {
    FLASH_TypeDef *reg;
};

flash_hal_handle_t *flash_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    flash_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (FLASH_TypeDef *)peripheral;
    return h;
}
void flash_hal_destroy(flash_hal_handle_t *h) { free(h); }

uint32_t flash_hal_sector_base(uint32_t sector)
{
    if (sector >= 12U) return 0U;
    return flash_sector_base_tbl[sector];
}

uint32_t flash_hal_sector_size(uint32_t sector)
{
    if (sector >= 12U) return 0U;
    return flash_sector_size_tbl[sector];
}

void flash_hal_unlock(flash_hal_handle_t *h)
{
    if (!h) return;
    if ((h->reg->CR & FLASH_CR_LOCK) != 0U) {
        h->reg->KEYR = FLASH_KEY1;
        h->reg->KEYR = FLASH_KEY2;
    }
}

static void flash_hal_wait_bsy(flash_hal_handle_t *h)
{
    while ((h->reg->SR & FLASH_SR_BSY) != 0U) { }
}

void flash_hal_erase_sector(flash_hal_handle_t *h, uint32_t sector)
{
    if (!h) return;
    flash_hal_unlock(h);
    flash_hal_wait_bsy(h);
    /* Clear PG/SER/MER, select sector-erase + sector number (SNB at bit 3),
     * then start. */
    h->reg->CR &= ~(FLASH_CR_PG | FLASH_CR_SER | FLASH_CR_MER);
    h->reg->CR |= FLASH_CR_SER | ((sector & 0x1FU) << 3);
    h->reg->CR |= FLASH_CR_STRT;
    flash_hal_wait_bsy(h);
    h->reg->CR &= ~(FLASH_CR_SER);
    h->reg->SR = h->reg->SR;   /* clear EOP / error W1C bits */
}

void flash_hal_program_u32(flash_hal_handle_t *h, uint32_t addr, uint32_t value)
{
    if (!h) return;
    flash_hal_unlock(h);
    flash_hal_wait_bsy(h);
    /* Select program mode with 32-bit parallelism (PSIZE = 2 at bits [9:8]). */
    h->reg->CR &= ~(FLASH_CR_SER | FLASH_CR_MER | (0x3U << 8));
    h->reg->CR |= FLASH_CR_PG | (0x2U << 8);
    *(volatile uint32_t *)addr = value;
    flash_hal_wait_bsy(h);
    h->reg->CR &= ~(FLASH_CR_PG);
    h->reg->SR = h->reg->SR;   /* clear EOP / error W1C bits */
}

uint32_t flash_hal_read_u32(flash_hal_handle_t *h, uint32_t addr)
{
    if (!h) return 0U;
    /* Invalidate the D-cache line covering this address so we read the real
     * flash value rather than a stale cached erase/old value. The STM32F407
     * has no data cache (__DCACHE_PRESENT == 0), so this is a no-op there; the
     * guard keeps the HAL portable to F7/H7 which do have a D-cache. */
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_InvalidateDCache_by_Addr((volatile void *)addr, 32);
#else
    __DSB();   /* ensure any prior program write is globally visible */
#endif
    return *(volatile uint32_t *)addr;
}

uint32_t flash_hal_get_status(flash_hal_handle_t *h)
{
    return h ? h->reg->SR : 0U;
}
