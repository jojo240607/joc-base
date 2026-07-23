#ifndef I2S_H
#define I2S_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* i2s IS-A stream_device (audio data stream) */
#include "i2s_hal.h"              /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"           /* pinmux_port_t */
#include <stdint.h>
#include <stddef.h>

/* device-level control commands for the I2S driver */
#define I2S_IOCTL_GET_I2SCFGR  0x01   /* arg: uint32_t* I2SCFGR register */
#define I2S_IOCTL_GET_I2SPR    0x02   /* arg: uint32_t* I2SPR register */
#define I2S_IOCTL_GET_PLLI2S   0x03   /* arg: uint32_t* RCC_PLLI2SCFGR */
#define I2S_IOCTL_GET_CFGR     0x04   /* arg: uint32_t* RCC_CFGR (I2SSRC) */
#define I2S_IOCTL_GET_PLL_RDY  0x05   /* arg: uint32_t* 1=PLLI2SRDY */
#define I2S_IOCTL_GET_AUDIO_HZ 0x06   /* arg: uint32_t* requested sample rate */
#define I2S_IOCTL_GET_I2S_CLK  0x07   /* arg: uint32_t* PLLI2S output Hz */

/*
 * Driver layer — generic I2S (STM32F4, SPI-hosted). Platform-independent: holds
 * ONLY an opaque `i2s_hal_handle_t *`. Implements the unified `device` interface
 * as a STREAM device (audio samples in / out). Only POLL mode is supported.
 */
typedef struct _i2s i2s;

struct _i2s {
    stream_device parent;         /* unified interface — MUST be first member (IS-A stream_device) */
    i2s_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    void *periph;                 /* SPI2 / SPI3 (the I2S host) — board layer only */
    uint32_t pclk_hz;             /* APB clock for register access (e.g. 42 MHz) */
    uint32_t audio_hz;            /* target sample rate */
    int master;                   /* 1 = master (clock generator) */
    int tx;                       /* 1 = transmit direction */
    uint32_t datlen;              /* 0=16,1=24,2=32 bit */
    /* resolved pin geometry (claimed at open) */
    pinmux_port_t ws_port;  uint8_t ws_pin;  uint8_t ws_af;
    pinmux_port_t ck_port;  uint8_t ck_pin;  uint8_t ck_af;
    pinmux_port_t sd_port;  uint8_t sd_pin;  uint8_t sd_af;
    pinmux_port_t esd_port; uint8_t esd_pin; uint8_t esd_af;
    int has_esd;
};

device *i2s_create(const void *config);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; i2s_create() reads it. */
typedef struct {
    const char *name;          /* logical device name */
    void *periph;              /* SPI2 / SPI3 (the I2S host) */
    uint32_t pclk_hz;          /* APB clock for register access */
    uint32_t audio_hz;         /* target sample rate (e.g. 48000) */
    const char *ws_signal;     /* I2Sx_WS  (word select / LRCLK) */
    const char *ck_signal;     /* I2Sx_CK  (bit clock) */
    const char *sd_signal;     /* I2Sx_SD  (data out in TX) */
    const char *extsd_signal;  /* I2Sx_extSD (data in in RX), may be NULL */
    int master;                /* 1 = master */
    int tx;                    /* 1 = transmit */
    uint32_t datlen;           /* 0=16,1=24,2=32 bit */
} i2s_config_t;

#endif /* I2S_H */
