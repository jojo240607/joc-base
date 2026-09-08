#include "flash_hal.h"
#include "stm32f103xx.h"
#include <stdlib.h>

/* FLASH key-register unlock magic values (write-only). */
#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU

/* STM32F103RC (256 KB) page base addresses (1 KB per page). */
#define F103_PAGE_SIZE  1024U
#define F103_NUM_PAGES  256U

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

uint32_t flash_hal_page_base(uint32_t page)
{
    if (page >= F103_NUM_PAGES) return 0U;
    return FLASH_BASE + page * F103_PAGE_SIZE;
}

void flash_hal_unlock(flash_hal_handle_t *h)
{
    if (!h) return;
    if ((h->reg->CR & FLASH_CR_LOCK) != 0U) {
        h->reg->KEYR = FLASH_KEY1;
        h->reg->KEYR = FLASH_KEY2;
    }
}

void flash_hal_lock(flash_hal_handle_t *h)
{
    if (h) h->reg->CR |= FLASH_CR_LOCK;
}

static void flash_hal_wait_bsy(flash_hal_handle_t *h)
{
    while ((h->reg->SR & FLASH_SR_BSY) != 0U) { }
}

void flash_hal_erase_page(flash_hal_handle_t *h, uint32_t page)
{
    if (!h) return;
    flash_hal_unlock(h);
    flash_hal_wait_bsy(h);

    /* F103 page erase: CR.PER=1, AR = page base address, STRT=1 */
    h->reg->CR &= ~(FLASH_CR_PG | FLASH_CR_MER);
    h->reg->CR |= FLASH_CR_PER;
    h->reg->AR = flash_hal_page_base(page);
    h->reg->CR |= FLASH_CR_STRT;
    flash_hal_wait_bsy(h);
    h->reg->CR &= ~FLASH_CR_PER;
    h->reg->SR = h->reg->SR;   /* clear EOP / error W1C bits */
}

void flash_hal_mass_erase(flash_hal_handle_t *h)
{
    if (!h) return;
    flash_hal_unlock(h);
    flash_hal_wait_bsy(h);

    h->reg->CR &= ~(FLASH_CR_PG | FLASH_CR_PER);
    h->reg->CR |= FLASH_CR_MER;
    h->reg->CR |= FLASH_CR_STRT;
    flash_hal_wait_bsy(h);
    h->reg->CR &= ~FLASH_CR_MER;
    h->reg->SR = h->reg->SR;
}

void flash_hal_program_u32(flash_hal_handle_t *h, uint32_t addr, uint32_t value)
{
    if (!h) return;
    flash_hal_unlock(h);
    flash_hal_wait_bsy(h);

    h->reg->CR &= ~(FLASH_CR_PER | FLASH_CR_MER);
    h->reg->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)addr = value;
    flash_hal_wait_bsy(h);
    h->reg->CR &= ~FLASH_CR_PG;
    h->reg->SR = h->reg->SR;
}

uint32_t flash_hal_read_u32(flash_hal_handle_t *h, uint32_t addr)
{
    if (!h) return 0U;
    __DSB();
    return *(volatile uint32_t *)addr;
}

uint32_t flash_hal_get_status(flash_hal_handle_t *h)
{
    return h ? h->reg->SR : 0U;
}

/* --- F4-compatible sector API (thin wrapper over page API) ---
 * F103 has no sectors; we map sector N to page N for compatibility. */
uint32_t flash_hal_sector_base(uint32_t sector)
{
    return flash_hal_page_base(sector);
}

uint32_t flash_hal_sector_size(uint32_t sector)
{
    return (sector < F103_NUM_PAGES) ? F103_PAGE_SIZE : 0U;
}

void flash_hal_erase_sector(flash_hal_handle_t *h, uint32_t sector)
{
    flash_hal_erase_page(h, sector);
}