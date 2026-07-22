#ifndef DAC_H
#define DAC_H

#include "iface/device.h"
#include "iface/stream_device.h"  /* dac IS-A stream_device (output stream) */
#include "dac_hal.h"              /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"           /* pinmux_port_t (resolved port for the pinmux claim) */
#include <stdint.h>

/* device-level control commands for the DAC driver */
#define DAC_IOCTL_SET_VALUE 0x60   /* arg: uint16_t* (12-bit value to output) */
#define DAC_IOCTL_GET_VALUE 0x61   /* arg: uint16_t* (read DOR register) */
#define DAC_IOCTL_GET_CR    0x62   /* arg: uint32_t* (read CR register) */

/*
 * Driver layer — generic DAC (STM32F4 DAC1, channels 1/2). Platform-independent:
 * it holds ONLY an opaque `dac_hal_handle_t *` and never references DAC_TypeDef.
 */
typedef struct _dac dac;

struct _dac {
    stream_device parent;         /* unified interface — MUST be first member */
    dac_hal_handle_t *hal;        /* opaque — driver never dereferences it */
    uint32_t channel;            /* 1 or 2 */
    const char *out_signal;      /* cached output signal name (e.g. "DAC1_OUT_PA4") */
    pinmux_port_t port;          /* resolved port for pinmux claim */
    uint8_t pin;                 /* resolved pin for pinmux claim */
    uint8_t af;                  /* resolved af (0 for analog) */
};

device *dac_create(const void *config);
void dac_destroy(dac *self);

/* Driver-specific board config — defined HERE, filled by the board. */
typedef struct {
    const char *name;       /* logical device name */
    void *periph;           /* DAC (board layer only) */
    uint32_t channel;       /* 1 or 2 */
    /* Output signal name to claim, supplied by the board. The pinmux resolves
     * it (e.g. "DAC1_OUT_PA4" -> PA4, af=0, analog) to the exact pad, so a pin
     * conflict is rejected before the analog GPIO register is touched. */
    const char *out_signal; /* e.g. "DAC1_OUT_PA4" */
} dac_config_t;

#endif /* DAC_H */
