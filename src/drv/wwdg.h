#ifndef WWDG_H
#define WWDG_H

#include "iface/device.h"
#include "iface/control_device.h"   /* WWDG IS-A control_device (parameter/state) */
#include "hal/stm32/wwdg_hal.h"
#include <stdint.h>

/* device-level control commands for the WWDG driver */
#define WWDG_IOCTL_SET_PRESCALER 0x60   /* arg: uint32_t* (WDGTB code 0..3) */
#define WWDG_IOCTL_SET_WINDOW    0x61   /* arg: uint32_t* (window 0x40..0x7F) */
#define WWDG_IOCTL_GET_CONFIG    0x62   /* arg: uint32_t* (raw CFR) */
#define WWDG_IOCTL_START         0x63   /* arg: uint32_t* (reload counter 0x40..0x7F) — ARM */
#define WWDG_IOCTL_REFRESH       0x64   /* arg: uint32_t* (reload counter > window) */
#define WWDG_IOCTL_GET_COUNTER   0x65   /* arg: uint32_t* (raw CR.T) */
#define WWDG_IOCTL_GET_STATUS    0x66   /* arg: uint32_t* (raw SR) */

/*
 * Driver layer — generic STM32 WWDG. Platform-independent: it keeps only the
 * ioctl surface and delegates ALL register work to the HAL. Implements the
 * unified `device` interface as a CONTROL-class device (a window watchdog /
 * safety timer, parameter/state surface).
 *
 * The driver does NOT auto-start the counter on open(); the application must
 * explicitly issue WWDG_IOCTL_START to arm it and keep issuing
 * WWDG_IOCTL_REFRESH (counter in the valid window) or the chip resets.
 * open() only gates the APB1 clock.
 */
typedef struct _wwdg wwdg;

struct _wwdg {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    wwdg_hal_handle_t *hal;
};

device *wwdg_create(const void *config);
void wwdg_destroy(wwdg *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; wwdg_create() reads it. */
typedef struct {
    const char *name;    /* logical device name */
    void *periph;        /* WWDG peripheral base (e.g. (void *)WWDG) */
} wwdg_config_t;

#endif /* WWDG_H */
