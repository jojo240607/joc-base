#include "spi_hal.h"
#include "stm32h750xx.h"   /* SPI_TypeDef, RCC, SPIx_BASE + bit defs */
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — SPI (STM32H7 v3, master, full-duplex).
 *
 * H7 SPIv3 register layout: CR1/CR2/CFG1/CFG2/IER/SR/IFCR/TXDR/RXDR.
 * Master mode, CPOL=0 CPHA=0 (mode 0), MSB-first, 8-bit data frame,
 * software NSS (SSM=1). Supports both POLLING and INTERRUPT-driven transfers.
 */

/* Per-wait timeout (tight-loop iterations). */
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
    else if (p == (void *)SPI2_BASE) RCC->APB1LENR |= RCC_APB1LENR_SPI2EN;
    else if (p == (void *)SPI3_BASE) RCC->APB1LENR |= RCC_APB1LENR_SPI3EN;
    else if (p == (void *)SPI4_BASE) RCC->APB2ENR |= RCC_APB2ENR_SPI4EN;
    else if (p == (void *)SPI5_BASE) RCC->APB2ENR |= RCC_APB2ENR_SPI5EN;
    else if (p == (void *)SPI6_BASE) { /* SPI6 on APB4 — no RCC gate defined yet */ }
}

/* Compute CFG1.MBR (baud-rate prescaler) from pclk_hz and target baud_hz.
 * H7 v3 MBR: 0=/2, 1=/4, 2=/8, 3=/16, 4=/32, 5=/64, 6=/128, 7=/256.
 * Returns the 3-bit field value (shifted to bit 28). */
static uint32_t spi_hal_mbr_field(uint32_t pclk_hz, uint32_t baud_hz)
{
    uint32_t div = 2;
    uint32_t mbr = 0;
    while (mbr < 7 && (pclk_hz / div) > baud_hz) {
        div <<= 1;
        mbr++;
    }
    return (mbr & 0x7UL) << 28;
}

void spi_hal_config(spi_hal_handle_t *h, uint32_t pclk_hz, uint32_t baud_hz)
{
    if (!h) return;
    SPI_TypeDef *r = h->reg;

    /* CR1: SPE = 0 during config, SSI = 1 (master NSS high), MASRX = 0 */
    r->CR1 = SPI_CR1_SSI;

    /* CR2: TSIZE = 0 (set per-transfer) */
    r->CR2 = 0;

    /* CFG1: 8-bit data (DSIZE=0x7), MBR from clock calc */
    r->CFG1 = (0x7UL & SPI_CFG1_DSIZE_Msk) | spi_hal_mbr_field(pclk_hz, baud_hz);

    /* CFG2: master, software NSS, CPOL=0, CPHA=0, SSM=1, SSI=1 */
    r->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM;

    /* IER: no interrupts enabled by default */
    r->IER = 0;

    /* Clear any stale flags */
    r->IFCR = SPI_IFCR_EOTC | SPI_IFCR_TXTFC;

    /* Enable SPI */
    r->CR1 |= SPI_CR1_SPE;
}

void spi_hal_set_peripheral_enable(spi_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->CR1 |= SPI_CR1_SPE; else h->reg->CR1 &= ~SPI_CR1_SPE;
}

int spi_hal_transfer(spi_hal_handle_t *h, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint16_t len)
{
    if (!h || len == 0) return -1;
    SPI_TypeDef *r = h->reg;
    volatile uint32_t tmo;

    /* Set transfer size in CR2 */
    r->CR2 = (r->CR2 & ~SPI_CR2_TSIZE_Msk)
           | ((uint32_t)len << SPI_CR2_TSIZE_Pos);

    /* CSTART (CR1 bit 9) starts the transfer on v3 */
    r->CR1 |= (1U << 9U);

    for (uint16_t i = 0; i < len; i++) {
        /* Wait TXP (TX data register empty) */
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_TXP)) { if (--tmo == 0) return -1; }
        r->TXDR = tx_buf ? tx_buf[i] : (uint8_t)0xFF;

        /* Wait RXP (RX data register not empty) */
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_RXP)) { if (--tmo == 0) return -1; }
        uint8_t d = (uint8_t)r->RXDR;
        if (rx_buf) rx_buf[i] = d;
    }

    /* Wait for EOT (end of transfer) */
    tmo = SPI_TIMEOUT;
    while (!(r->SR & SPI_SR_EOT)) { if (--tmo == 0) break; }
    /* Clear EOT */
    r->IFCR |= SPI_IFCR_EOTC;

    return 0;
}

/* --- Interrupt mode helpers --- */

irq_id_t spi_hal_irq_id(spi_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)SPI1_BASE) return (irq_id_t)SPI1_IRQn;
    else if (p == (void *)SPI2_BASE) return (irq_id_t)SPI2_IRQn;
    else if (p == (void *)SPI3_BASE) return (irq_id_t)SPI3_IRQn;
    else if (p == (void *)SPI4_BASE) return (irq_id_t)SPI4_IRQn;
    else if (p == (void *)SPI5_BASE) return (irq_id_t)SPI5_IRQn;
    else if (p == (void *)SPI6_BASE) return (irq_id_t)SPI6_IRQn;
    return -1;
}

void spi_hal_enable_rxne_irq(spi_hal_handle_t *h)
    { if (h) h->reg->IER |= SPI_IER_RXPIE; }

void spi_hal_disable_rxne_irq(spi_hal_handle_t *h)
    { if (h) h->reg->IER &= ~SPI_IER_RXPIE; }

void spi_hal_write_dr(spi_hal_handle_t *h, uint8_t data)
    { if (h) *(volatile uint8_t *)&h->reg->TXDR = data; }

uint8_t spi_hal_read_dr(spi_hal_handle_t *h)
    { return h ? (uint8_t)h->reg->RXDR : 0U; }

/* H7 v3 DMA gating is in CFG1 (not CR2 like F4). */
void spi_hal_enable_tx_dma(spi_hal_handle_t *h)  { if (h) h->reg->CFG1 |= SPI_CFG1_TXDMAEN; }
void spi_hal_disable_tx_dma(spi_hal_handle_t *h) { if (h) h->reg->CFG1 &= ~SPI_CFG1_TXDMAEN; }
void spi_hal_enable_rx_dma(spi_hal_handle_t *h)  { if (h) h->reg->CFG1 |= SPI_CFG1_RXDMAEN; }
void spi_hal_disable_rx_dma(spi_hal_handle_t *h) { if (h) h->reg->CFG1 &= ~SPI_CFG1_RXDMAEN; }
void *spi_hal_get_dr_addr(spi_hal_handle_t *h)   { return h ? (void *)&h->reg->TXDR : NULL; }

/* readback helpers */
uint32_t spi_hal_get_cr1(spi_hal_handle_t *h) { return h ? h->reg->CR1 : 0UL; }
int spi_hal_is_busy(spi_hal_handle_t *h) { return h ? ((h->reg->SR & SPI_SR_EOT) ? 0 : 1) : 0; }