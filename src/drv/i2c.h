#ifndef I2C_H
#define I2C_H

#include "iface/stream_device.h"
#include "iface/device.h"
#include "i2c_hal.h"
#include "drv/dma.h"              /* dma / dma_stream_t (DMA engine) + dma_req_id_t */
#include "pinmux_hal.h"
#include "osal/osal.h"
#include "irq.h"
#include <stdint.h>

/*
 * I2C driver — a STREAM device wrapping the F1-style I2C in MASTER mode.
 *
 * Supports three transfer engines via STREAM_IOCTL_SET_MODE:
 *   POLL — CPU spins on SR1 flags (TXE/RXNE/ADDR/BTF), timeout-guarded.
 *   IRQ  — EV+ER ISRs drive the F1 I2C state machine; thread blocks on a
 *          completion flag.
 *   DMA  — START/address handshake stays on the CPU (timeout-guarded), but the
 *          byte movement is offloaded to the DMA controller (CR2.DMAEN). For a
 *          multi-byte RX the hardware auto-NACKs the final byte (CR2.LAST).
 *
 * The IRQ-only transfer state (completion flag + state machine scratch) and the
 * DMA-only state (reserved TX/RX streams) live in per-engine structs reached via
 * a single `eng` pointer, so a POLL I2C pays nothing. stream_read()/write() use
 * the current slave address (I2C_IOCTL_SET_ADDR); multi-address use the addressed
 * ioctls (i2c_xfer_t carries an explicit 7-bit address).
 */
typedef struct _i2c i2c;

/* Per-engine state for the IRQ engine: the completion flag + state-machine
 * scratch. NULL for POLL/DMA. Allocated on SET_MODE(IRQ), freed on close. */
typedef struct {
    volatile int xfer_done;
    volatile uint8_t irq_state;
    volatile int irq_result;
    uint16_t addr;
    const uint8_t *volatile tx_buf;
    uint8_t *volatile rx_buf;
    volatile uint16_t xfer_len;
    volatile uint16_t xfer_pos;
} i2c_irq_t;

/* Per-engine state for the DMA engine: the reserved TX (M2P) and RX (P2M)
 * streams. NULL for POLL/IRQ. Allocated on SET_MODE(DMA), freed on close. */
typedef struct {
    dma *dma_dev;          /* controller owning the acquired streams (dma1 for I2C) */
    dma_stream_t *dma_tx;  /* data out  (M2P) */
    dma_stream_t *dma_rx;  /* data in   (P2M) */
} i2c_dma_t;

typedef struct {
    const char *name;
    void *peripheral;
    uint32_t clk_hz;
    uint32_t speed_hz;
    const char *scl_signal;
    const char *sda_signal;
    /* Logical DMA request ids (e.g. DMA_REQ_I2C1_TX / DMA_REQ_I2C1_RX). The
     * driver resolves each to a concrete (controller, stream, channel) via
     * dma_hal_route(); DMA_REQ_NONE means that direction has no DMA (the driver
     * refuses STREAM_MODE_DMA if either direction is missing). */
    dma_req_id_t dma_tx_req;
    dma_req_id_t dma_rx_req;
} i2c_config_t;

struct _i2c {
    stream_device parent;
    i2c_hal_handle_t *hal;
    uint32_t clk_hz;
    uint32_t speed_hz;
    uint16_t current_addr;       /* 7-bit slave address for stream_read/write */
    pinmux_port_t scl_port, sda_port;
    uint8_t  scl_pin, sda_pin, scl_af, sda_af;
    int       ev_irq;
    int       er_irq;
    dma_req_id_t dma_tx_req;     /* cached from config */
    dma_req_id_t dma_rx_req;     /* cached from config */
    void *eng;                   /* per-engine: i2c_irq_t* / i2c_dma_t* / NULL(POLL) */
};

device *i2c_create(const void *config);
void i2c_destroy(i2c *self);

/* ioctl commands */
#define I2C_IOCTL_MASTER_WRITE  0x30   /* arg = i2c_xfer_t* (addressed write) */
#define I2C_IOCTL_MASTER_READ   0x31   /* arg = i2c_xfer_t* (addressed read) */
#define I2C_IOCTL_BUS_SCAN      0x32   /* arg = i2c_scan_t* */
#define I2C_IOCTL_SET_SPEED     0x33   /* arg = uint32_t* */
#define I2C_IOCTL_GET_CCR        0x37   /* arg = uint32_t* */
#define I2C_IOCTL_GET_CR2_FREQ   0x38   /* arg = uint32_t* */
#define I2C_IOCTL_GET_CR1        0x35
#define I2C_IOCTL_GET_BUSY       0x36
#define I2C_IOCTL_SET_ADDR       0x39   /* arg = uint16_t* (current slave addr) */
#define I2C_IOCTL_GET_ADDR       0x3a   /* arg = uint16_t* */

typedef struct {
    uint16_t addr;       /* 7-bit slave address */
    uint8_t *buf;
    uint16_t len;
    int      result;     /* OUT: 0 = ACK, -1 = NACK/timeout */
} i2c_xfer_t;

typedef struct {
    uint8_t  acks[128];
    uint16_t found;
} i2c_scan_t;

#endif /* I2C_H */
