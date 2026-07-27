#ifndef SPI_H
#define SPI_H

#include "iface/stream_device.h"   /* spi IS-A stream_device (read/write POLL/IRQ) */
#include "iface/device.h"           /* DEVICE_IOCTL_*, driver_type_t */
#include "spi_hal.h"                /* opaque HAL handle */
#include "drv/dma.h"                /* dma / dma_stream_t (DMA engine) + dma_req_id_t */
#include "pinmux_hal.h"             /* pinmux_port_t */
#include "osal/osal.h"              /* osal_sem_t (IRQ completion) */
#include "irq.h"                    /* irq_id_t */
#include <stdint.h>

/* DMA-accessible (main SRAM) bounce scratch for the SPI DMA TX/RX paths. The DMA
 * controllers cannot reach CCM (0x10000000) — only the CPU can — so caller
 * buffers on the CCM stack are invalid as DMA source/destination. The spi struct
 * is malloc'd in main SRAM, so this member is safe; we DMA through it. */
#define SPI_DMA_BOUNCE 256

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

/* Per-engine state — heap-allocated on demand in open()/SET_MODE and freed in
 * close(). Only the ACTIVE engine's variant exists at any time, so the RAM it
 * costs is paid only when that engine is actually used. This replaces the old
 * static layout where EVERY spi paid for an xfer_done semaphore + 2x256 B DMA
 * bounce + ISR transfer state up-front even when it ran POLL/IRQ — a POLL or
 * IRQ spi now costs ~0 B of engine state instead of ~0.5 KB.
 *
 * SPI has a single orthogonal axis here: the transfer ENGINE (POLL/IRQ/DMA).
 * There is no second (framing-like) axis, so the variants are simply keyed by
 * parent.mode; the active one is reached through a single `p->eng` cast. */
typedef struct {
    osal_sem_t xfer_done;            /* signaled by the RXNE ISR on completion */
    const uint8_t *tx_buf;           /* ISR advances these (IRQ mode only) */
    uint8_t *rx_buf;
    volatile uint16_t xfer_len;
    volatile uint16_t xfer_pos;
} spi_irq_t;

typedef struct {
    /* main-SRAM scratch (DMA cannot touch CCM). The TX source and RX dest must
     * be separate buffers (full-duplex: TX src + RX dst must not overlap). */
    uint8_t dma_bounce[SPI_DMA_BOUNCE];
    uint8_t dma_rx_bounce[SPI_DMA_BOUNCE];
} spi_dma_t;

/* Board fills this as DATA. pclk_hz is the APB clock (84 MHz SPI1, 42 MHz SPI2/3). */
typedef struct {
    const char *name;
    void *peripheral;
    uint32_t pclk_hz;
    uint32_t baud_hz;
    const char *sck_signal;
    const char *miso_signal;
    const char *mosi_signal;
    /* DMA request IDs (logical, from dma_hal.h). The driver resolves each to a
     * concrete (controller, stream, channel) via dma_hal_route(). 0 means "no
     * DMA for this direction" (the driver then refuses STREAM_MODE_DMA). */
    dma_req_id_t dma_tx_req;
    dma_req_id_t dma_rx_req;
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

    /* DMA engine handles (valid only when engine == STREAM_MODE_DMA and the
     * streams were successfully acquired at open). Kept as always-present small
     * pointers so TX/RX can arm a transfer without re-resolving the route. The
     * large DMA bounce buffers live in the per-engine spi_dma_t instead. */
    dma_req_id_t dma_tx_req;      /* cached from config */
    dma_req_id_t dma_rx_req;
    dma *dma_dev;                 /* resolved dma controller (dma1/dma2) */
    dma_stream_t *dma_tx;         /* reserved TX stream handle (NULL if none) */
    dma_stream_t *dma_rx;         /* reserved RX stream handle (NULL if none) */

    /* per-engine state — heap-allocated in open() for the chosen engine, freed in
     * close(). NULL for POLL (zero state); the active variant is selected by
     * parent.mode (spi_irq_t for IRQ, spi_dma_t for DMA). */
    void *eng;
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
