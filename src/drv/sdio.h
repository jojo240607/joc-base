#ifndef SDIO_H
#define SDIO_H

#include "iface/stream_device.h"
#include "sdio_hal.h"
#include "pinmux_hal.h"
#include <stdint.h>

/*
 * SDIO driver — STREAM device for the STM32 SDIO host peripheral.
 *
 * This is a PURE INTERFACE driver: it owns the pins and the SDIO peripheral,
 * but does NOT implement SD protocol logic. Higher-level drivers (sd_card)
 * use sdio_hal directly or send raw commands through ioctl.
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

struct _sdio {
    stream_device parent;
    sdio_hal_handle_t *hal;
    pinmux_port_t ck_port, cmd_port, d0_port, d1_port, d2_port, d3_port;
    uint8_t  ck_pin, cmd_pin, d0_pin, d1_pin, d2_pin, d3_pin;
    uint8_t  ck_af, cmd_af, d0_af, d1_af, d2_af, d3_af;
};

device *sdio_create(const void *config);
void sdio_destroy(sdio *self);

/* ioctl */
#define SDIO_IOCTL_CMD         0x50   /* arg = sdio_cmd_t* (raw command) */
#define SDIO_IOCTL_SET_CLOCK   0x51   /* arg = uint32_t* (clkdiv) */
#define SDIO_IOCTL_GET_POWER   0x54
#define SDIO_IOCTL_GET_CLKCR   0x55

typedef struct {
    uint32_t index;
    uint32_t arg;
    uint32_t resp_type;   /* 0=none, 1=short, 2=long */
    uint32_t resp[4];     /* OUT */
} sdio_cmd_t;

#endif /* SDIO_H */
