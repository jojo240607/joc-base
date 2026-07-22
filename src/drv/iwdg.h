#ifndef IWDG_H
#define IWDG_H

#include "iface/device.h"
#include "iface/control_device.h"   /* IWDG IS-A control_device (parameter/state) */
#include "hal/stm32/iwdg_hal.h"
#include <stdint.h>

/* device-level control commands for the IWDG driver */
#define IWDG_IOCTL_SET_PRESCALER 0x60   /* arg: uint32_t* (prescaler code 0..7) */
#define IWDG_IOCTL_SET_RELOAD    0x61   /* arg: uint32_t* (reload 0..4095) */
#define IWDG_IOCTL_GET_PRESCALER 0x62   /* arg: uint32_t* */
#define IWDG_IOCTL_GET_RELOAD    0x63   /* arg: uint32_t* */
#define IWDG_IOCTL_START         0x64   /* arg: none — ARM (resets if unfed) */
#define IWDG_IOCTL_REFRESH       0x65   /* arg: none — feed the counter */
#define IWDG_IOCTL_GET_STATUS    0x66   /* arg: uint32_t* (raw SR) */

/*
 * Driver layer — generic STM32 IWDG. Platform-independent: it keeps only the
 * ioctl surface and delegates ALL register work to the HAL. Implements the
 * unified `device` interface as a CONTROL-class device (a watchdog / safety
 * timer, parameter/state surface).
 *
 * IMPORTANT: the driver does NOT auto-start the counter on open(); the
 * application must explicitly issue IWDG_IOCTL_START to arm it, and must keep
 * issuing IWDG_IOCTL_REFRESH (or a systick callback that does) or the chip
 * will reset. open() only makes sure LSI is running.
 */
typedef struct _iwdg iwdg;

struct _iwdg {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    iwdg_hal_handle_t *hal;
};

device *iwdg_create(const void *config);
void iwdg_destroy(iwdg *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; iwdg_create() reads it. */
typedef struct {
    const char *name;    /* logical device name */
    void *periph;        /* IWDG peripheral base (e.g. (void *)IWDG) */
} iwdg_config_t;

#endif /* IWDG_H */
