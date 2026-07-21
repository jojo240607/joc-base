#ifndef I2C_H
#define I2C_H

#include "iface/stream_device.h"
#include "iface/device.h"
#include "i2c_hal.h"
#include "pinmux_hal.h"
#include "osal/osal.h"
#include "irq.h"
#include <stdint.h>

/*
 * I2C driver — a STREAM device wrapping the F1-style I2C in MASTER mode.
 *
 * Supports POLL and IRQ modes via STREAM_IOCTL_SET_MODE:
 *   POLL — CPU spins on SR1 flags (TXE/RXNE/ADDR/BTF), timeout-guarded.
 *   IRQ  — EV+ER ISRs drive the F1 I2C state machine; thread blocks on
 *          a completion flag. (NVIC enable still hangs from i2c.c, so IRQ
 *          mode activates only peripheral interrupts — NVIC enable pending.)
 *
 * stream_read() / stream_write() use the current slave address (set via
 * I2C_IOCTL_SET_ADDR). For multi-address operation use the addressed ioctls:
 *   I2C_IOCTL_MASTER_WRITE / I2C_IOCTL_MASTER_READ carry an i2c_xfer_t with
 *   an explicit 7-bit slave address.
 */
typedef struct _i2c i2c;

typedef struct {
    const char *name;
    void *peripheral;
    uint32_t clk_hz;
    uint32_t speed_hz;
    const char *scl_signal;
    const char *sda_signal;
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
    volatile int xfer_done;      /* completion flag (IRQ mode) */

    /* IRQ transfer state */
    volatile uint8_t  irq_state;
    volatile int      irq_result;
    uint16_t addr;
    const uint8_t *volatile tx_buf;
    uint8_t *volatile rx_buf;
    volatile uint16_t xfer_len;
    volatile uint16_t xfer_pos;
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
