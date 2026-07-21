#ifndef SD_CARD_H
#define SD_CARD_H

#include "iface/block_device.h"
#include "sdio_hal.h"
#include "pinmux_hal.h"
#include <stdint.h>

/*
 * SD Card driver — a BLOCK device that implements the SD protocol over
 * the SDIO bus. Uses sdio_hal directly (does NOT go through the sdio
 * STREAM device). This is a higher-level protocol driver, analogous to
 * how temp_sensor uses adc_hal directly.
 */
typedef struct _sd_card sd_card;

typedef struct {
    const char *name;          /* logical name */
    void *peripheral;          /* SDIO base */
    const char *ck_signal;     /* SDIO_CK */
    const char *cmd_signal;    /* SDIO_CMD */
    const char *d0_signal;
    const char *d1_signal;
    const char *d2_signal;
    const char *d3_signal;
} sd_card_config_t;

struct _sd_card {
    block_device parent;
    sdio_hal_handle_t *hal;
    pinmux_port_t ck_port,cmd_port,d0_port,d1_port,d2_port,d3_port;
    uint8_t ck_pin,cmd_pin,d0_pin,d1_pin,d2_pin,d3_pin;
    uint8_t ck_af,cmd_af,d0_af,d1_af,d2_af,d3_af;
    uint16_t rca;
    uint8_t  csd[16], cid[16];
    uint32_t block_len;
    uint32_t card_size;    /* in 512-byte blocks */
    int      card_type;    /* 0=none,1=SDSC,2=SDHC */
    int      ready;
};

device *sd_card_create(const void *config);
void sd_card_destroy(sd_card *self);

#endif /* SD_CARD_H */
