#ifndef SPI_HAL_H
#define SPI_HAL_H

#include <stdint.h>
#include "irq.h"

/*
 * Hardware Abstraction Layer — SPI (STM32F1, MASTER mode, full-duplex).
 *
 * F1 SPI is register-compatible with F4 (CR1/CR2/SR/DR). Master mode,
 * CPOL=0 CPHA=0 (mode 0), MSB-first, 8-bit data frame, software NSS.
 * All register knowledge stays here; the driver only sees the opaque handle.
 */
typedef struct spi_hal_handle spi_hal_handle_t;

spi_hal_handle_t *spi_hal_create(void *peripheral);
void spi_hal_destroy(spi_hal_handle_t *h);

void spi_hal_enable_clock(spi_hal_handle_t *h);
void spi_hal_config(spi_hal_handle_t *h, uint32_t pclk_hz, uint32_t baud_hz);
void spi_hal_set_peripheral_enable(spi_hal_handle_t *h, int on);

int spi_hal_transfer(spi_hal_handle_t *h, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint16_t len);

irq_id_t spi_hal_irq_id(spi_hal_handle_t *h);
void     spi_hal_enable_rxne_irq(spi_hal_handle_t *h);
void     spi_hal_disable_rxne_irq(spi_hal_handle_t *h);
void     spi_hal_write_dr(spi_hal_handle_t *h, uint8_t data);
uint8_t  spi_hal_read_dr(spi_hal_handle_t *h);

void     spi_hal_enable_tx_dma(spi_hal_handle_t *h);
void     spi_hal_disable_tx_dma(spi_hal_handle_t *h);
void     spi_hal_enable_rx_dma(spi_hal_handle_t *h);
void     spi_hal_disable_rx_dma(spi_hal_handle_t *h);
void    *spi_hal_get_dr_addr(spi_hal_handle_t *h);

uint32_t spi_hal_get_cr1(spi_hal_handle_t *h);
int      spi_hal_is_busy(spi_hal_handle_t *h);

#endif /* SPI_HAL_H */