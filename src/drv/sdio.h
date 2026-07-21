#ifndef SDIO_H
#define SDIO_H

#include "iface/stream_device.h"
#include "iface/device.h"
#include "sdio_hal.h"
#include "pinmux_hal.h"
#include <stdint.h>

/*
 * SDIO driver — a STREAM device wrapping the STM32 SDIO host peripheral.
 *
 * The driver configures the SDIO bus (pins, clock, power, bus width) and
 * implements the SD protocol (card init, single/multi-block read/write).
 * It is STREAM because it moves a continuous stream of data over the bus.
 *
 * SD card state and protocol logic live here (like I2C slave protocol
 * lives in the I2C driver). A future BLOCK device abstraction can be
 * layered on top by consuming the SDIO device through device_manager.
 */
typedef struct _sdio sdio;

typedef struct {
    const char *name;
    void *peripheral;
    const char *ck_signal;
    const char *cmd_signal;
    const char *d0_signal;
    const char *d1_signal;
    const char *d2_signal;
    const char *d3_signal;
} sdio_config_t;

typedef struct {
    uint16_t rca;
    uint8_t  csd[16];
    uint8_t  cid[16];
    uint32_t block_len;
    uint32_t card_size;       /* in 512-byte blocks */
    int      card_type;       /* 0=none, 1=SDSC, 2=SDHC/SDXC */
    int      ready;
} sdio_card_info_t;

struct _sdio {
    stream_device parent;
    sdio_hal_handle_t *hal;
    sdio_card_info_t card;
    pinmux_port_t ck_port, cmd_port, d0_port, d1_port, d2_port, d3_port;
    uint8_t  ck_pin, cmd_pin, d0_pin, d1_pin, d2_pin, d3_pin;
    uint8_t  ck_af, cmd_af, d0_af, d1_af, d2_af, d3_af;
};

device *sdio_create(const void *config);
void sdio_destroy(sdio *self);

/* ioctl commands (BLOCK ops for SD card protocol) */
#define SDIO_IOCTL_INIT        0x50
#define SDIO_IOCTL_READ_BLOCK  0x51   /* arg = sdio_blk_t* */
#define SDIO_IOCTL_WRITE_BLOCK 0x52
#define SDIO_IOCTL_GET_INFO    0x53   /* arg = sdio_card_info_t* */
#define SDIO_IOCTL_GET_POWER   0x54
#define SDIO_IOCTL_GET_CLKCR   0x55

typedef struct {
    uint32_t block_addr;
    uint8_t *buf;
    uint32_t count;
    int      result;
} sdio_blk_t;

#endif /* SDIO_H */
