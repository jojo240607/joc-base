#ifndef I2C_H
#define I2C_H

#include "iface/control_device.h"   /* i2c IS-A control_device (command/set/get) */
#include "i2c_hal.h"                /* opaque HAL handle (driver never sees I2C_TypeDef) */
#include "pinmux_hal.h"             /* pinmux_port_t (resolved port for the pinmux claim) */
#include <stdint.h>

/*
 * I2C driver — a CONTROL device wrapping the F4 "new" I2C peripheral in MASTER
 * mode (polling, timeout-guarded — see i2c_hal.h for the errata rationale).
 *
 * The board supplies the real I2C base (I2C1/2/3) and the two AF signal names
 * for SCL/SDA (e.g. "I2C1_SCL_PB6" / "I2C1_SDA_PB7"). The driver claims both
 * pins through the pinmux (open-drain AF, pull-up), enables the peripheral
 * clock, and programs TIMINGR for the requested speed.
 *
 * Transfers are exposed as CONTROL commands (not the base read/write, which
 * have no address parameter): MASTER_WRITE / MASTER_READ carry an i2c_xfer_t
 * with the 7-bit slave address; BUS_SCAN probes every 7-bit address. This makes
 * the driver identical to use over the unified device/ioctl surface.
 */
typedef struct _i2c i2c;

/* Board fills this as DATA. clk_hz is PCLK1 (42 MHz on F4 APB1). */
typedef struct {
    const char *name;        /* logical device name (e.g. "i2c0") */
    void *peripheral;        /* I2C base (board supplies the real silicon) */
    uint32_t clk_hz;         /* PCLK1 feeding this I2C (42 MHz on F4) */
    uint32_t speed_hz;       /* 100000 (standard) or 400000 (fast) */
    const char *scl_signal;  /* AF signal name, e.g. "I2C1_SCL_PB6" */
    const char *sda_signal;  /* AF signal name, e.g. "I2C1_SDA_PB7" */
} i2c_config_t;

struct _i2c {
    control_device parent;       /* IS-A control_device IS-A device */
    i2c_hal_handle_t *hal;       /* opaque HAL handle */
    uint32_t clk_hz;             /* cached from config */
    uint32_t speed_hz;           /* cached from config */
    pinmux_port_t scl_port;      /* resolved SCL port for pinmux claim */
    uint8_t  scl_pin;            /* resolved SCL pin */
    uint8_t  scl_af;             /* resolved SCL AF */
    pinmux_port_t sda_port;      /* resolved SDA port */
    uint8_t  sda_pin;            /* resolved SDA pin */
    uint8_t  sda_af;             /* resolved SDA AF */
};

/* uniform create signature (device *(*)(const void *)) for the board node list */
device *i2c_create(const void *config);
void i2c_destroy(i2c *self);

/* ioctl / control commands (driver-specific) */
#define I2C_IOCTL_MASTER_WRITE  0x30   /* arg = i2c_xfer_t* (dir = write) */
#define I2C_IOCTL_MASTER_READ   0x31   /* arg = i2c_xfer_t* (dir = read) */
#define I2C_IOCTL_BUS_SCAN      0x32   /* arg = i2c_scan_t* (probe all addrs) */
#define I2C_IOCTL_SET_SPEED     0x33   /* arg = uint32_t* (Hz; reprograms TIMINGR) */
#define I2C_IOCTL_GET_TIMINGR   0x34   /* arg = uint32_t* (raw TIMINGR) */
#define I2C_IOCTL_GET_CR1       0x35   /* arg = uint32_t* (raw CR1) */
#define I2C_IOCTL_GET_BUSY      0x36   /* arg = int* (1 = bus busy) */
#define I2C_IOCTL_GET_CCR        0x37   /* arg = uint32_t* (raw CCR) */
#define I2C_IOCTL_GET_CR2_FREQ   0x38   /* arg = uint32_t* (CR2.FREQ) */

/* transfer descriptor for MASTER_WRITE / MASTER_READ.
 * len = 0 is a PROBE (emits START + address + STOP) — useful to detect a device
 * without exchanging data. result: 0 = ACK, -1 = NACK / timeout. */
typedef struct {
    uint16_t addr;       /* 7-bit slave address (0..127) */
    uint8_t *buf;        /* data buffer (may be NULL when len = 0) */
    uint16_t len;        /* byte count (0 = probe only) */
    int      result;     /* OUT: 0 = success (ACK), -1 = NACK / timeout */
} i2c_xfer_t;

/* bus-scan result: acks[addr] = 1 if the 7-bit address answered (ACK). */
typedef struct {
    uint8_t  acks[128];  /* indexed by 7-bit address 0..127 */
    uint16_t found;      /* count of ACKed addresses */
} i2c_scan_t;

#endif /* I2C_H */
