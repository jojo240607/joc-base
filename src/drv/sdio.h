#ifndef SDIO_H
#define SDIO_H

#include "iface/control_device.h"
#include "sdio_hal.h"
#include "pinmux_hal.h"
#include <stdint.h>

/*
 * SDIO driver — a CONTROL device wrapping the STM32 SDIO peripheral.
 *
 * Supports SD card initialization and block read/write in POLL mode.
 * The bus is 4-bit wide by default; fallback to 1-bit for compatibility.
 *
 * Card detection and data transfer are ioctl-driven. For a BLOCK device
 * abstraction, these will be extended later.
 */
typedef struct _sdio sdio;

typedef struct {
    const char *name;
    void *peripheral;         /* SDIO base */
    const char *ck_signal;    /* SDIO_CK */
    const char *cmd_signal;   /* SDIO_CMD */
    const char *d0_signal;    /* SDIO_D0 */
    const char *d1_signal;    /* SDIO_D1 */
    const char *d2_signal;    /* SDIO_D2 */
    const char *d3_signal;    /* SDIO_D3 */
} sdio_config_t;

/* SD card state (tracked by the driver) */
typedef struct {
    uint16_t rca;             /* relative card address */
    uint8_t  csd[16];         /* CSD register (128-bit) */
    uint8_t  cid[16];         /* CID register (128-bit) */
    uint32_t block_len;       /* block length in bytes (default 512) */
    uint32_t card_size;       /* total size in blocks */
    int      card_type;       /* 0=none, 1=SDSC, 2=SDHC/SDXC */
    int      ready;           /* 1 = card initialized */
} sdio_card_info_t;

struct _sdio {
    control_device parent;
    sdio_hal_handle_t *hal;
    sdio_card_info_t card;
    pinmux_port_t ck_port, cmd_port, d0_port, d1_port, d2_port, d3_port;
    uint8_t  ck_pin, cmd_pin, d0_pin, d1_pin, d2_pin, d3_pin;
    uint8_t  ck_af, cmd_af, d0_af, d1_af, d2_af, d3_af;
};

device *sdio_create(const void *config);
void sdio_destroy(sdio *self);

/* ioctl commands */
#define SDIO_IOCTL_INIT        0x50   /* arg = NULL — init card, fill card info */
#define SDIO_IOCTL_READ_BLOCK  0x51   /* arg = sdio_blk_t* */
#define SDIO_IOCTL_WRITE_BLOCK 0x52   /* arg = sdio_blk_t* */
#define SDIO_IOCTL_GET_INFO    0x53   /* arg = sdio_card_info_t* */
#define SDIO_IOCTL_GET_POWER   0x54   /* arg = uint32_t* */
#define SDIO_IOCTL_GET_CLKCR   0x55   /* arg = uint32_t* */

typedef struct {
    uint32_t block_addr;   /* block address (LBA for SDHC, byte/512 for SDSC) */
    uint8_t *buf;          /* data buffer */
    uint32_t count;        /* block count */
    int      result;       /* OUT: 0 = success */
} sdio_blk_t;

#endif /* SDIO_H */
