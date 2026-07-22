#ifndef RNG_H
#define RNG_H

#include "iface/device.h"
#include "iface/control_device.h"   /* RNG IS-A control_device (parameter/state) */
#include "hal/stm32/rng_hal.h"
#include <stdint.h>

/* device-level control commands for the RNG driver */
#define RNG_IOCTL_GET_U32   0x60   /* arg: uint32_t*  (one 32-bit random word) */
#define RNG_IOCTL_GET_STATUS 0x61  /* arg: uint32_t*  (raw RNG->SR) */

/*
 * Driver layer — generic STM32 RNG. Platform-independent: it keeps only the
 * ioctl surface and delegates ALL register work to the HAL. Switching chips =
 * rewrite the HAL only. Implements the unified `device` interface as a
 * CONTROL-class device (a source of random data, parameter/state surface).
 */
typedef struct _rng rng;

struct _rng {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    rng_hal_handle_t *hal;
};

device *rng_create(const void *config);
void rng_destroy(rng *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; rng_create() reads it. */
typedef struct {
    const char *name;    /* logical device name */
    void *periph;        /* RNG peripheral base (e.g. (void *)RNG) */
} rng_config_t;

#endif /* RNG_H */
