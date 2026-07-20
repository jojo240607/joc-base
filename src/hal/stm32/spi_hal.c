#include "spi_hal.h"
#include "stm32f4xx.h"     /* SPI_TypeDef, RCC, SPI1/2/3 base + bit defs */
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — SPI (STM32F4, master polling, full-duplex).
 */

/* Per-wait timeout (tight-loop iterations). At 168 MHz this is ~ a ms per
 * wait — long enough for a slow SPI byte (~8 us at 1 MHz), never hangs. */
#define SPI_TIMEOUT  200000U

struct spi_hal_handle {
    SPI_TypeDef *reg;
};

spi_hal_handle_t *spi_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    spi_hal_handle_t *h = (spi_hal_handle_t *)malloc(sizeof(spi_hal_handle_t));
    if (!h) return NULL;
    h->reg = (SPI_TypeDef *)peripheral;
    return h;
}

void spi_hal_destroy(spi_hal_handle_t *h) { free(h); }

void spi_hal_enable_clock(spi_hal_handle_t *h)
{
    if (!h) return;
    void *p = (void *)h->reg;
    if      (p == (void *)SPI1_BASE) RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    else if (p == (void *)SPI2_BASE) RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    else if (p == (void *)SPI3_BASE) RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;
}

/* Compute the BR[2:0] field (baud rate prescaler) so SCK <= baud_hz.
 * f_SCK = f_PCLK / 2^(BR+1). We pick the largest BR that keeps SCK <= target. */
static uint32_t spi_hal_br_field(uint32_t pclk_hz, uint32_t baud_hz)
{
    uint32_t div = 2;                   /* start at BR=0: div=2 */
    uint32_t br = 0;
    while (br < 7 && (pclk_hz / div) > baud_hz) {
        div <<= 1;                      /* div *= 2 */
        br++;
    }
    return (br & 0x7U) << 3;            /* BR[2:0] in CR1 bits 5:3 */
}

void spi_hal_config(spi_hal_handle_t *h, uint32_t pclk_hz, uint32_t baud_hz)
{
    if (!h) return;
    SPI_TypeDef *r = h->reg;
    uint32_t br = spi_hal_br_field(pclk_hz, baud_hz);
    /* Master mode: MSTR=1, software NSS (SSM=1, SSI=1), CPOL=0, CPHA=0,
     * 8-bit (DFF=0), MSB-first (LSBFIRST=0), full-duplex (BIDIMODE=0). */
    r->CR1 = br                          /* BR[2:0] */
           | SPI_CR1_MSTR                /* master */
           | SPI_CR1_SSM | SPI_CR1_SSI   /* software slave management, NSS high */
           | SPI_CR1_SPE;                /* enable */
}

void spi_hal_set_peripheral_enable(spi_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->CR1 |= SPI_CR1_SPE; else h->reg->CR1 &= ~SPI_CR1_SPE;
}

int spi_hal_transfer(spi_hal_handle_t *h, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint16_t len)
{
    if (!h) return -1;
    SPI_TypeDef *r = h->reg;
    volatile uint32_t tmo;

    for (uint16_t i = 0; i < len; i++) {
        /* Wait TXE (transmit buffer empty) */
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_TXE)) { if (--tmo == 0) return -1; }

        /* Write data (8-bit access to DR). */
        *(volatile uint8_t *)&r->DR = tx_buf ? tx_buf[i] : (uint8_t)0xFF;

        /* Wait RXNE (receive buffer not empty) */
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_RXNE)) { if (--tmo == 0) return -1; }

        /* Read data (8-bit access). */
        uint8_t d = (uint8_t)r->DR;
        if (rx_buf) rx_buf[i] = d;
    }

    /* Wait for BSY to clear (last byte fully shifted out). */
    tmo = SPI_TIMEOUT;
    while (r->SR & SPI_SR_BSY) { if (--tmo == 0) break; }

    return 0;
}

/* readback helpers */
uint32_t spi_hal_get_cr1(spi_hal_handle_t *h) { return h ? h->reg->CR1 : 0UL; }
int spi_hal_is_busy(spi_hal_handle_t *h) { return h ? ((h->reg->SR & SPI_SR_BSY) ? 1 : 0) : 0; }
