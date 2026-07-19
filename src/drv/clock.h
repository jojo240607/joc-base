#ifndef CLOCK_H
#define CLOCK_H

#include "iface/device.h"
#include "iface/control_device.h"  /* clock IS-A control_device (parameter/state) */
#include <stdint.h>

/* device-level control commands for the clock driver */
#define CLK_IOCTL_GET_SYSCLK_HZ  0x01   /* arg: uint32_t* hz */

/*
 * Driver layer — generic system clock. Already platform-independent: it keeps
 * only a logical `sysclk_hz` and delegates all register work to the HAL
 * (hal/<platform>/clock_hal). Switching chips = rewrite the HAL only.
 * Implements the unified `device` interface.
 */
/* NOTE: the type is named `sys_clock` (not `clock`) on purpose — `clock()` is a
 * reserved identifier in <time.h>, and newlib's stdio chain can pull it in. */
typedef struct _sys_clock sys_clock;

struct clockFun {
    void (*destroy)(sys_clock *self);
    void (*init)(sys_clock *self);
    void (*deinit)(sys_clock *self);
    uint32_t (*get_sysclk_hz)(sys_clock *self);
};

struct _sys_clock {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    const struct clockFun *fun;
    uint32_t sysclk_hz;
};

device *clock_create(const void *config);
void clock_destroy(sys_clock *self);
void clock_init(sys_clock *self);
void clock_deinit(sys_clock *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; clock_create() reads it. */
typedef struct {
    const char *name;       /* logical device name */
} clock_config_t;

extern const struct clockFun clock_fun;

#endif /* CLOCK_H */
