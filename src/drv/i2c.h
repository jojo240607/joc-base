#ifndef I2C_H
#define I2C_H

#include "iface/control_device.h"
#include "i2c_hal.h"
#include "pinmux_hal.h"
#include <stdint.h>

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
    control_device parent;
    i2c_hal_handle_t *hal;
    uint32_t clk_hz;
    uint32_t speed_hz;
    pinmux_port_t scl_port, sda_port;
    uint8_t  scl_pin, sda_pin, scl_af, sda_af;
};

device *i2c_create(const void *config);
void i2c_destroy(i2c *self);

#define I2C_IOCTL_MASTER_WRITE  0x30
#define I2C_IOCTL_MASTER_READ   0x31
#define I2C_IOCTL_BUS_SCAN      0x32
#define I2C_IOCTL_SET_SPEED     0x33
#define I2C_IOCTL_GET_TIMINGR   0x34
#define I2C_IOCTL_GET_CR1       0x35
#define I2C_IOCTL_GET_BUSY      0x36
#define I2C_IOCTL_GET_CCR        0x37
#define I2C_IOCTL_GET_CR2_FREQ   0x38

typedef struct { uint16_t addr; uint8_t *buf; uint16_t len; int result; } i2c_xfer_t;
typedef struct { uint8_t acks[128]; uint16_t found; } i2c_scan_t;

#endif /* I2C_H */
