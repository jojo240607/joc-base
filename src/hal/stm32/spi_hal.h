#ifndef SPI_HAL_H
#define SPI_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — SPI (MASTER mode, POLLING, full-duplex).
 *
 * STM32F4 SPI is the standard SPI peripheral (CR1/CR2/SR/DR). Master mode,
 * CPOL=0 CPHA=0 (mode 0), MSB-first, 8-bit data frame, software NSS.
 * All register knowledge stays here; the driver only sees the opaque handle.
 *
 * The driver is POLLING (no IRQ): every wait has a hard timeout so a stuck
 * bus can never wedge the CPU.
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

/* Full-duplex polling transfer. For each of len bytes: wait TXE, write DR
 * (tx byte or 0xFF if tx_buf NULL), wait RXNE, read DR (into rx_buf if not
 * NULL). Returns 0 on success, -1 on timeout. */
int spi_hal_transfer(spi_hal_handle_t *h, const uint8_t *tx_buf,
                     uint8_t *rx_buf, uint16_t len);

/* readback helpers for self-test verification */
uint32_t spi_hal_get_cr1(spi_hal_handle_t *h);
int      spi_hal_is_busy(spi_hal_handle_t *h);  /* SR.BSY */

#endif /* SPI_HAL_H */
