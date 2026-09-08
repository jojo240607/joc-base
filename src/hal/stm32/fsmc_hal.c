#include "fsmc_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

/* FSMC 寄存器区基址（CMSIS FSMC_R_BASE = 0xA0000000）。 */
#define FSMC_BANK1_REG  ((FSMC_Bank1_TypeDef *)FSMC_R_BASE)
#define FSMC_BANK1E_REG ((FSMC_Bank1E_TypeDef *)(FSMC_R_BASE + 0x104UL))
/* Bank1 片选窗口映射基址（真机外部存储器总线；模拟器 64KB 后备缓冲）。 */
#define FSMC_BANK1_WIN  0x60000000UL

struct fsmc_hal_handle {
    FSMC_Bank1_TypeDef *b1;
    FSMC_Bank1E_TypeDef *b1e;
};

fsmc_hal_handle_t *fsmc_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    fsmc_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->b1  = (FSMC_Bank1_TypeDef *)peripheral;
    h->b1e = (FSMC_Bank1E_TypeDef *)((uint8_t *)peripheral + 0x104UL);
    return h;
}

void fsmc_hal_destroy(fsmc_hal_handle_t *h)
{
    if (h) free(h);
}

void fsmc_hal_enable_clock(fsmc_hal_handle_t *h)
{
    if (!h) return;
    RCC->AHB3ENR |= RCC_AHB3ENR_FSMCEN;   /* gate FSMC on AHB3 */
}

uint32_t fsmc_hal_get_bcr(fsmc_hal_handle_t *h, int bank)
{
    if (!h || bank < 1 || bank > 4) return 0;
    return h->b1->BTCR[(bank - 1) * 2];   /* BCR1 @BTCR[0], BCR2 @BTCR[2], ... */
}

void fsmc_hal_set_bcr(fsmc_hal_handle_t *h, int bank, uint32_t val)
{
    if (!h || bank < 1 || bank > 4) return;
    h->b1->BTCR[(bank - 1) * 2] = val;
}

uint32_t fsmc_hal_get_btr(fsmc_hal_handle_t *h, int bank)
{
    if (!h || bank < 1 || bank > 4) return 0;
    return h->b1->BTCR[(bank - 1) * 2 + 1];  /* BTR1 @BTCR[1], BTR2 @BTCR[3], ... */
}

void fsmc_hal_set_btr(fsmc_hal_handle_t *h, int bank, uint32_t val)
{
    if (!h || bank < 1 || bank > 4) return;
    h->b1->BTCR[(bank - 1) * 2 + 1] = val;
}

uint32_t fsmc_hal_get_bwtr(fsmc_hal_handle_t *h, int bank)
{
    if (!h || bank < 1 || bank > 4) return 0;
    return h->b1e->BWTR[bank - 1];
}

void fsmc_hal_bank1_enable(fsmc_hal_handle_t *h)
{
    if (!h) return;
    h->b1->BTCR[0] |= FSMC_BCR1_MBKEN;
}

int fsmc_hal_bank1_enabled(fsmc_hal_handle_t *h)
{
    if (!h) return 0;
    return (h->b1->BTCR[0] & FSMC_BCR1_MBKEN) != 0;
}

int fsmc_hal_bank1_read32(fsmc_hal_handle_t *h, uint32_t off, uint32_t *v)
{
    if (!h || !v || off >= 0x10000UL) return -1;
    /* 仅 Bank1 使能后窗口可访问（未选通读回 0 是模拟器/总线语义）。 */
    if (!fsmc_hal_bank1_enabled(h)) return -2;
    volatile uint32_t *p = (volatile uint32_t *)(FSMC_BANK1_WIN + off);
    *v = *p;
    return 0;
}

int fsmc_hal_bank1_write32(fsmc_hal_handle_t *h, uint32_t off, uint32_t v)
{
    if (!h || off >= 0x10000UL) return -1;
    if (!fsmc_hal_bank1_enabled(h)) return -2;
    volatile uint32_t *p = (volatile uint32_t *)(FSMC_BANK1_WIN + off);
    *p = v;
    return 0;
}
