#ifndef SPI_HAL_H
#define SPI_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/*
 * Hardware Abstraction Layer — SPI (STM32F4, MASTER mode, full-duplex).
 *
 * STM32F4 SPI is the standard SPI peripheral (CR1/CR2/SR/DR). Master mode,
 * CPOL=0 CPHA=0 (mode 0), MSB-first, 8-bit data frame, software NSS.
 * Supports both POLLING and INTERRUPT-driven transfers.
 * All register knowledge stays here; the driver only sees the opaque handle.
 */
typedef struct spi_hal_handle spi_hal_handle_t;

spi_hal_handle_t *spi_hal_create(void *peripheral);
void spi_hal_destroy(spi_hal_handle_t *h);

void spi_hal_enable_clock(spi_hal_handle_t *h);
/* Program CR1 for baud_hz (target SCK frequency). pclk_hz = APB clock
 * (84 MHz for SPI1 on APB2, 42 MHz for SPI2/3 on APB1). Sets MSTR=1,
 * SSM=1, SSI=1, CPOL=0, CPHA=0, 8-bit, MSB-first, SPE=1. */
void spi_hal_config(spi_hal_handle_t *h, uint32_t pclk_hz, uint32_t baud_hz);
void spi_hal_set_peripheral_enable(spi_hal_handle_t *h, int on); /* CR1.SPE */

/* POLLING: full-duplex transfer. For each byte: wait TXE, write DR, wait
 * RXNE, read DR. Returns 0 on success, -1 on timeout. */
int spi_hal_transfer(spi_hal_handle_t *h, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint16_t len);

/* INTERRUPT: low-level helpers used by the driver's ISR + transfer logic
 * (the driver registers the IRQ handler and manages transfer state). */
irq_id_t spi_hal_irq_id(spi_hal_handle_t *h);       /* chip IRQn for this SPI */
void     spi_hal_enable_rxne_irq(spi_hal_handle_t *h);  /* CR2.RXNEIE */
void     spi_hal_disable_rxne_irq(spi_hal_handle_t *h); /* CR2.RXNEIE */
void     spi_hal_write_dr(spi_hal_handle_t *h, uint8_t data);
uint8_t  spi_hal_read_dr(spi_hal_handle_t *h);

/* readback helpers for self-test verification */
uint32_t spi_hal_get_cr1(spi_hal_handle_t *h);
int      spi_hal_is_busy(spi_hal_handle_t *h);  /* SR.BSY */

#endif /* SPI_HAL_H */
