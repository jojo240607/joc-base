#ifndef SPI_H
#define SPI_H

#include "iface/stream_device.h"   /* spi IS-A stream_device (read/write POLL/IRQ) */
#include "iface/device.h"           /* DEVICE_IOCTL_*, driver_type_t */
#include "spi_hal.h"                /* opaque HAL handle */
#include "pinmux_hal.h"             /* pinmux_port_t */
#include "osal/osal.h"              /* osal_sem_t (IRQ completion) */
#include "irq.h"                    /* irq_id_t */
#include <stdint.h>

/*
 * SPI driver — a STREAM device wrapping the STM32 SPI peripheral in MASTER mode.
 *
 * Supports two transfer engines selected via STREAM_IOCTL_SET_MODE:
 *   STREAM_MODE_POLL — CPU spins on TXE/RXNE flags (existing).
 *   STREAM_MODE_IRQ  — RXNEIE interrupt drives the data; the driver busy-waits
 *                       on a completion semaphore (interrupts enabled) so the
 *                       ISR runs. The semaphore is signaled when the last byte
 *                       is received.
 *
 * Full-duplex transfers are exposed as SPI_IOCTL_XFER (carrying tx_buf, rx_buf
 * and len). For read-only, set tx_buf=NULL (sends 0xFF dummies); for write-only,
 * set rx_buf=NULL (discards received data).
 */
typedef struct _spi spi;

/* Board fills this as DATA. pclk_hz is the APB clock (84 MHz SPI1, 42 MHz SPI2/3). */
typedef struct {
    const char *name;
    void *peripheral;
    uint32_t pclk_hz;
    uint32_t baud_hz;
    const char *sck_signal;
    const char *miso_signal;
    const char *mosi_signal;
} spi_config_t;

struct _spi {
    stream_device parent;        /* IS-A stream_device IS-A device */
    spi_hal_handle_t *hal;
    uint32_t pclk_hz;
    uint32_t baud_hz;
    pinmux_port_t sck_port, miso_port, mosi_port;
    uint8_t  sck_pin, miso_pin, mosi_pin;
    uint8_t  sck_af, miso_af, mosi_af;
    irq_id_t  irq;               /* platform IRQ number */
    osal_sem_t xfer_done;        /* completion semaphore for IRQ mode */

    /* transfer state (used by ISR in IRQ mode) */
    const uint8_t *volatile tx_buf;
    uint8_t *volatile rx_buf;
    volatile uint16_t xfer_len;
    volatile uint16_t xfer_pos;
};

device *spi_create(const void *config);
void spi_destroy(spi *self);

/* ioctl / control commands */
#define SPI_IOCTL_XFER        0x40   /* arg = spi_xfer_t* (full-duplex, both modes) */
#define SPI_IOCTL_GET_CR1     0x41   /* arg = uint32_t* (raw CR1) */
#define SPI_IOCTL_GET_BSY     0x42   /* arg = int* (1 = busy) */

/* transfer descriptor: full-duplex or half-duplex. */
typedef struct {
    const uint8_t *tx_buf;   /* transmit data (NULL = send 0xFF) */
    uint8_t *rx_buf;         /* receive data (NULL = discard) */
    uint16_t len;
} spi_xfer_t;

#endif /* SPI_H */
