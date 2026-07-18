#ifndef CLOCK_H
#define CLOCK_H

#include "iface/device.h"
#include <stdint.h>

/* device-level control commands for the clock driver */
#define CLK_IOCTL_GET_SYSCLK_HZ  0x01   /* arg: uint32_t* hz */

/*
 * Driver layer — generic system clock. Already platform-independent: it keeps
 * only a logical `sysclk_hz` and delegates all register work to the HAL
 * (hal/<platform>/clock_hal). Switching chips = rewrite the HAL only.
 * Implements the unified `device` interface.
 */
typedef struct _clock clock;

struct clockFun {
    void (*destroy)(clock *self);
    void (*init)(clock *self);
    void (*deinit)(clock *self);
    uint32_t (*get_sysclk_hz)(clock *self);
};

struct _clock {
    device parent;                /* unified interface — MUST be first member */
    const struct clockFun *fun;
    uint32_t sysclk_hz;
};

device *clock_create(const void *config);
void clock_destroy(clock *self);
void clock_init(clock *self);
void clock_deinit(clock *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; clock_create() reads it. */
typedef struct {
    const char *name;       /* logical device name */
} clock_config_t;

extern const struct clockFun clock_fun;

#endif /* CLOCK_H */
