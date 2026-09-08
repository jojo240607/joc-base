#include "sdio_hal.h"
#include "stm32h750xx.h"
#include <stdlib.h>

#define SDIO_TIMEOUT  200000U

struct sdio_hal_handle {
    SDMMC_TypeDef *reg;
};

sdio_hal_handle_t *sdio_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    sdio_hal_handle_t *h = malloc(sizeof(*h));
    if (!h) return NULL;
    h->reg = (SDMMC_TypeDef *)peripheral;
    return h;
}
void sdio_hal_destroy(sdio_hal_handle_t *h) { free(h); }

void sdio_hal_enable_clock(sdio_hal_handle_t *h) { if (h) RCC->AHB3ENR |= RCC_AHB3ENR_SDMMC1EN; }
void sdio_hal_power_up(sdio_hal_handle_t *h)   { if (h) h->reg->POWER = SDMMC_POWER_PWRCTRL_0 | SDMMC_POWER_PWRCTRL_1; }
void sdio_hal_power_down(sdio_hal_handle_t *h)  { if (h) h->reg->POWER = 0; }
void sdio_hal_set_clock_div(sdio_hal_handle_t *h, uint32_t clkdiv)
    { if (h) { h->reg->CLKCR = (h->reg->CLKCR & ~SDMMC_CLKCR_CLKDIV_Msk) | (clkdiv & SDMMC_CLKCR_CLKDIV_Msk); } }
void sdio_hal_enable_ck(sdio_hal_handle_t *h, int on)
    { if (h) { if (on) h->reg->CLKCR |= SDMMC_CLKCR_CLKEN; else h->reg->CLKCR &= ~SDMMC_CLKCR_CLKEN; } }
void sdio_hal_set_bus_width(sdio_hal_handle_t *h, int width)
{
    if (!h) return;
    uint32_t v = h->reg->CLKCR & ~SDMMC_CLKCR_WIDBUS_Msk;
    if (width == 4) v |= SDMMC_CLKCR_WIDBUS_0;
    h->reg->CLKCR = v;
}

int sdio_hal_cmd(sdio_hal_handle_t *h, uint32_t idx, uint32_t arg,
                 uint32_t resp_type, uint32_t *resp)
{
    if (!h) return -1;
    SDMMC_TypeDef *r = h->reg;
    volatile uint32_t tmo;

    /* clear status */
    r->ICR = 0xFFFFFFFFU;

    /* set argument and command */
    r->ARG = arg;
    uint32_t cmd = (idx & SDMMC_CMD_CMDINDEX_Msk) | SDMMC_CMD_CPSMEN;
    if (resp_type == 1) cmd |= SDMMC_CMD_WAITRESP_0;        /* short: WAITRESP=01 */
    else if (resp_type == 2) cmd |= SDMMC_CMD_WAITRESP_Msk; /* long:  WAITRESP=11 */
    r->CMD = cmd;

    /* wait for CMDREND (command response received) or CMDSENT (no response) or error */
    uint32_t wait = SDMMC_STA_CMDREND | SDMMC_STA_CTIMEOUT | SDMMC_STA_CCRCFAIL;
    if (resp_type == 0) wait |= SDMMC_STA_CMDSENT;  /* no response => CMDSENT */
    tmo = SDIO_TIMEOUT;
    while (!(r->STA & wait)) { if (--tmo == 0) return -1; }

    if (r->STA & (SDMMC_STA_CTIMEOUT | SDMMC_STA_CCRCFAIL)) {
        r->ICR = SDMMC_ICR_CTIMEOUTC | SDMMC_ICR_CCRCFAILC;
        return -1;
    }

    /* read response */
    if (resp_type == 1 && resp) {
        resp[0] = r->RESP1;
        resp[1] = 0;  /* short response fits in one word */
    }
    if (resp_type == 2 && resp) {
        resp[0] = r->RESP1;
        resp[1] = r->RESP2;
        resp[2] = r->RESP3;
        resp[3] = r->RESP4;  /* long R2 response: 4 words (136-bit) */
    }

    r->ICR = SDMMC_ICR_CMDRENDC | SDMMC_ICR_CMDSENTC;
    return 0;
}

void sdio_hal_data_config(sdio_hal_handle_t *h, uint32_t dir,
                          uint32_t blk_size, uint32_t count)
{
    if (!h) return;
    h->reg->DTIMER = 0xFFFFFFFFU;                      /* max timeout */
    h->reg->DLEN = blk_size * count;
    uint32_t blocksize_bits = 0;
    for (uint32_t s = blk_size; s > 1; s >>= 1) blocksize_bits++;
    h->reg->DCTRL = (dir ? SDMMC_DCTRL_DTDIR : 0)
                  | ((blocksize_bits << 4) & SDMMC_DCTRL_DBLOCKSIZE_Msk)
                  | SDMMC_DCTRL_DTEN;
}

void sdio_hal_data_enable(sdio_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->DCTRL |= SDMMC_DCTRL_DTEN;
    else    h->reg->DCTRL &= ~SDMMC_DCTRL_DTEN;
}

void sdio_hal_read_fifo(sdio_hal_handle_t *h, uint32_t *buf, int word_count)
{
    if (!h || !buf) return;
    for (int i = 0; i < word_count; i++) buf[i] = h->reg->FIFO;
}

void sdio_hal_write_fifo(sdio_hal_handle_t *h, const uint32_t *buf, int word_count)
{
    if (!h || !buf) return;
    for (int i = 0; i < word_count; i++) h->reg->FIFO = buf[i];
}

uint32_t sdio_hal_get_sta(sdio_hal_handle_t *h) { return h ? h->reg->STA : 0U; }
void sdio_hal_clear_icr(sdio_hal_handle_t *h, uint32_t mask)
    { if (h) h->reg->ICR = mask; }

int sdio_hal_wait_sta(sdio_hal_handle_t *h, uint32_t mask, uint32_t timeout)
{
    if (!h) return 0;
    while (timeout--) {
        uint32_t sta = h->reg->STA;
        if (sta & mask) return 1;
    }
    return 0;
}

int sdio_hal_wait_data_end(sdio_hal_handle_t *h, uint32_t timeout)
{
    if (!h) return -1;
    while (timeout--) {
        uint32_t sta = h->reg->STA;
        if (sta & SDMMC_STA_DATAEND) return 0;
        if (sta & (SDMMC_STA_DTIMEOUT | SDMMC_STA_DCRCFAIL)) {
            h->reg->ICR = 0xFFFFFFFFU; return -1;
        }
    }
    return -1;
}

void sdio_hal_data_config_dma(sdio_hal_handle_t *h, uint32_t dir,
                              uint32_t blk_size, uint32_t count)
{
    if (!h) return;
    sdio_hal_data_config(h, dir, blk_size, count);   /* sets DTEN + DTDIR */
    h->reg->DCTRL |= SDMMC_DCTRL_DMAEN;
}

void sdio_hal_dma_enable(sdio_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->DCTRL |= SDMMC_DCTRL_DMAEN;
    else    h->reg->DCTRL &= ~SDMMC_DCTRL_DMAEN;
}

void *sdio_hal_get_fifo_addr(sdio_hal_handle_t *h)
    { return h ? (void *)&h->reg->FIFO : NULL; }

void sdio_hal_clear_data_icr(sdio_hal_handle_t *h)
{
    if (h) h->reg->ICR = SDMMC_ICR_DATAENDC | SDMMC_ICR_DCRCFAILC | SDMMC_ICR_DTIMEOUTC;
}

int sdio_hal_read_block(sdio_hal_handle_t *h, uint8_t *buf,
                        uint32_t blk_addr, uint32_t count, int is_sdhc)
{
    if (!h || !buf) return -1;
    uint32_t addr = is_sdhc ? blk_addr : (blk_addr * 512);
    sdio_hal_data_config(h, 0, 512, count);
    if (sdio_hal_cmd(h, (count == 1) ? 17 : 18, addr, 1, NULL)) return -1;

    for (uint32_t blk = 0; blk < count; blk++) {
        if (!sdio_hal_wait_sta(h, SDMMC_STA_RXDAVL | SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT, 500000))
            { h->reg->ICR = 0xFFFFFFFF; return -1; }
        if (h->reg->STA & (SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT))
            { h->reg->ICR = 0xFFFFFFFF; return -1; }
        sdio_hal_read_fifo(h, (uint32_t *)(buf + blk * 512), 128);
    }
    if (!sdio_hal_wait_sta(h, SDMMC_STA_DATAEND | SDMMC_STA_DTIMEOUT, 500000))
        { h->reg->ICR = 0xFFFFFFFF; return -1; }
    if (h->reg->STA & SDMMC_STA_DTIMEOUT) { h->reg->ICR = 0xFFFFFFFF; return -1; }
    h->reg->ICR = SDMMC_ICR_DATAENDC | SDMMC_ICR_DCRCFAILC | SDMMC_ICR_DTIMEOUTC;
    if (count > 1) sdio_hal_stop_transfer(h);
    return 0;
}

int sdio_hal_write_block(sdio_hal_handle_t *h, const uint8_t *buf,
                         uint32_t blk_addr, uint32_t count, int is_sdhc)
{
    if (!h || !buf) return -1;
    uint32_t addr = is_sdhc ? blk_addr : (blk_addr * 512);
    sdio_hal_data_config(h, 1, 512, count);
    if (sdio_hal_cmd(h, (count == 1) ? 24 : 25, addr, 1, NULL)) return -1;

    for (uint32_t blk = 0; blk < count; blk++) {
        if (!sdio_hal_wait_sta(h, SDMMC_STA_TXFIFOE | SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT, 500000))
            { h->reg->ICR = 0xFFFFFFFF; return -1; }
        if (h->reg->STA & (SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT))
            { h->reg->ICR = 0xFFFFFFFF; return -1; }
        sdio_hal_write_fifo(h, (const uint32_t *)(buf + blk * 512), 128);
    }
    if (!sdio_hal_wait_sta(h, SDMMC_STA_DATAEND | SDMMC_STA_DTIMEOUT, 500000))
        { h->reg->ICR = 0xFFFFFFFF; return -1; }
    if (h->reg->STA & SDMMC_STA_DTIMEOUT) { h->reg->ICR = 0xFFFFFFFF; return -1; }
    h->reg->ICR = SDMMC_ICR_DATAENDC | SDMMC_ICR_DCRCFAILC | SDMMC_ICR_DTIMEOUTC;
    if (count > 1) sdio_hal_stop_transfer(h);
    return 0;
}

int sdio_hal_stop_transfer(sdio_hal_handle_t *h)
{
    if (!h) return -1;
    uint32_t r1;
    return sdio_hal_cmd(h, 12, 0, 1, &r1);
}

uint32_t sdio_hal_get_power(sdio_hal_handle_t *h) { return h ? h->reg->POWER : 0U; }
uint32_t sdio_hal_get_clkcr(sdio_hal_handle_t *h) { return h ? h->reg->CLKCR : 0U; }