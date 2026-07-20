#ifndef SPI_H
#define SPI_H

#include "iface/control_device.h"   /* spi IS-A control_device (command/set/get) */
#include "spi_hal.h"                /* opaque HAL handle (driver never sees SPI_TypeDef) */
#include "pinmux_hal.h"             /* pinmux_port_t (resolved port for the pinmux claim) */
#include <stdint.h>

/*
 * SPI driver — a CONTROL device wrapping the STM32 SPI peripheral in MASTER
 * mode (polling, full-duplex via ioctl).
 *
 * The board supplies the real SPI base (SPI1/2/3), the APB clock feeding it,
 * and the three AF signal names for SCK/MISO/MOSI. The driver claims all three
 * pins through the pinmux (AF push-pull, very-high speed), enables the peripheral
 * clock, and programs CR1 for the requested baud rate (master, mode 0, 8-bit,
 * software NSS).
 *
 * Full-duplex transfers are exposed as a CONTROL command carrying tx_buf, rx_buf
 * and len. For read-only, set tx_buf=NULL (sends 0xFF dummies); for write-only,
 * set rx_buf=NULL (discards received data).
 */
typedef struct _spi spi;

/* Board fills this as DATA. pclk_hz is the APB clock (84 MHz SPI1, 42 MHz SPI2/3). */
typedef struct {
    const char *name;        /* logical device name (e.g. "spi0") */
    void *peripheral;        /* SPI base (board supplies the real silicon) */
    uint32_t pclk_hz;        /* APB clock feeding this SPI */
    uint32_t baud_hz;        /* target SCK frequency (Hz) */
    const char *sck_signal;  /* AF signal name, e.g. "SPI1_SCK_PA5" */
    const char *miso_signal; /* AF signal name, e.g. "SPI1_MISO_PA6" */
    const char *mosi_signal; /* AF signal name, e.g. "SPI1_MOSI_PA7" */
    /* NSS is handled by software (SSM=1, SSI=1); no pin needed. */
} spi_config_t;

struct _spi {
    control_device parent;       /* IS-A control_device IS-A device */
    spi_hal_handle_t *hal;       /* opaque HAL handle */
    uint32_t pclk_hz;            /* cached from config */
    uint32_t baud_hz;            /* cached from config */
    pinmux_port_t sck_port;      /* resolved SCK port */
    uint8_t  sck_pin;            /* resolved SCK pin */
    uint8_t  sck_af;             /* resolved SCK AF */
    pinmux_port_t miso_port;     /* resolved MISO port */
    uint8_t  miso_pin;           /* resolved MISO pin */
    uint8_t  miso_af;            /* resolved MISO AF */
    pinmux_port_t mosi_port;     /* resolved MOSI port */
    uint8_t  mosi_pin;           /* resolved MOSI pin */
    uint8_t  mosi_af;            /* resolved MOSI AF */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *spi_create(const void *config);
void spi_destroy(spi *self);

/* ioctl / control commands */
#define SPI_IOCTL_XFER        0x40   /* arg = spi_xfer_t* (full-duplex) */
#define SPI_IOCTL_GET_CR1     0x41   /* arg = uint32_t* (raw CR1) */
#define SPI_IOCTL_GET_BSY     0x42   /* arg = int* (1 = busy) */

/* transfer descriptor: for each byte, transmits tx_byte or 0xFF if tx_buf NULL,
 * receives into rx_buf if not NULL (else discarded). */
typedef struct {
    const uint8_t *tx_buf;   /* transmit data (NULL = send 0xFF) */
    uint8_t *rx_buf;         /* receive data (NULL = discard) */
    uint16_t len;            /* byte count */
} spi_xfer_t;

#endif /* SPI_H */
