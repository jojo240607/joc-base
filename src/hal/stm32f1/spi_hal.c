/*
 * SPI hardware abstraction layer for STM32F103.
 *
 * F1 SPI is register-compatible with F4 (CR1/CR2/SR/DR). Adapted from the
 * stm32/ (F4) version with stm32f1xx.h includes and F1 clock-enable bits.
 *
 * F103 has SPI1 (APB2) and SPI2 (APB1), no SPI3.
 */
#include "spi_hal.h"
#include "stm32f1xx.h"
#include <stdlib.h>

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
}

static uint32_t spi_hal_br_field(uint32_t pclk_hz, uint32_t baud_hz)
{
    uint32_t div = 2;
    uint32_t br = 0;
    while (br < 7 && (pclk_hz / div) > baud_hz) {
        div <<= 1;
        br++;
    }
    return (br & 0x7U) << 3;
}

void spi_hal_config(spi_hal_handle_t *h, uint32_t pclk_hz, uint32_t baud_hz)
{
    if (!h) return;
    SPI_TypeDef *r = h->reg;
    uint32_t br = spi_hal_br_field(pclk_hz, baud_hz);
    r->CR1 = br | SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE;
    r->CR2 = 0;
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
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_TXE)) { if (--tmo == 0) return -1; }
        *(volatile uint8_t *)&r->DR = tx_buf ? tx_buf[i] : (uint8_t)0xFF;
        tmo = SPI_TIMEOUT;
        while (!(r->SR & SPI_SR_RXNE)) { if (--tmo == 0) return -1; }
        uint8_t d = (uint8_t)r->DR;
        if (rx_buf) rx_buf[i] = d;
    }
    tmo = SPI_TIMEOUT;
    while (r->SR & SPI_SR_BSY) { if (--tmo == 0) break; }
    return 0;
}

irq_id_t spi_hal_irq_id(spi_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)SPI1_BASE) return (irq_id_t)SPI1_IRQn;
    else if (p == (void *)SPI2_BASE) return (irq_id_t)SPI2_IRQn;
    return -1;
}

void spi_hal_enable_rxne_irq(spi_hal_handle_t *h)
    { if (h) h->reg->CR2 |= SPI_CR2_RXNEIE; }

void spi_hal_disable_rxne_irq(spi_hal_handle_t *h)
    { if (h) h->reg->CR2 &= ~SPI_CR2_RXNEIE; }

void spi_hal_write_dr(spi_hal_handle_t *h, uint8_t data)
    { if (h) *(volatile uint8_t *)&h->reg->DR = data; }

uint8_t spi_hal_read_dr(spi_hal_handle_t *h)
    { return h ? (uint8_t)h->reg->DR : 0U; }

void spi_hal_enable_tx_dma(spi_hal_handle_t *h)  { if (h) h->reg->CR2 |= SPI_CR2_TXDMAEN; }
void spi_hal_disable_tx_dma(spi_hal_handle_t *h) { if (h) h->reg->CR2 &= ~SPI_CR2_TXDMAEN; }
void spi_hal_enable_rx_dma(spi_hal_handle_t *h)  { if (h) h->reg->CR2 |= SPI_CR2_RXDMAEN; }
void spi_hal_disable_rx_dma(spi_hal_handle_t *h) { if (h) h->reg->CR2 &= ~SPI_CR2_RXDMAEN; }
void *spi_hal_get_dr_addr(spi_hal_handle_t *h)   { return h ? (void *)&h->reg->DR : NULL; }

uint32_t spi_hal_get_cr1(spi_hal_handle_t *h) { return h ? h->reg->CR1 : 0UL; }
int spi_hal_is_busy(spi_hal_handle_t *h) { return h ? ((h->reg->SR & SPI_SR_BSY) ? 1 : 0) : 0; }