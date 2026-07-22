#include "crc_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

struct crc_hal_handle {
    CRC_TypeDef *reg;
};

crc_hal_handle_t *crc_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    crc_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (CRC_TypeDef *)peripheral;
    return h;
}
void crc_hal_destroy(crc_hal_handle_t *h) { free(h); }

void crc_hal_enable(crc_hal_handle_t *h)
{
    (void)h;
    RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
}

void crc_hal_reset(crc_hal_handle_t *h)
{
    if (!h) return;
    /* Writing 1 to CRC_CR.RESET resets DR to 0xFFFFFFFF; the bit reads back 0. */
    h->reg->CR = CRC_CR_RESET;
}

void crc_hal_update_u32(crc_hal_handle_t *h, uint32_t word)
{
    if (!h) return;
    h->reg->DR = word;
}

void crc_hal_update(crc_hal_handle_t *h, const uint32_t *buf, size_t nwords)
{
    if (!h || !buf) return;
    for (size_t i = 0; i < nwords; i++) h->reg->DR = buf[i];
}

uint32_t crc_hal_result(crc_hal_handle_t *h)
{
    return h ? h->reg->DR : 0U;
}
