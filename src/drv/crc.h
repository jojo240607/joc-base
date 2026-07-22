#ifndef CRC_H
#define CRC_H

#include "iface/device.h"
#include "iface/control_device.h"   /* CRC IS-A control_device (parameter/state) */
#include "hal/stm32/crc_hal.h"
#include <stdint.h>

/* device-level control commands for the CRC driver */
#define CRC_IOCTL_RESET   0x60   /* arg: none — start a new message */
#define CRC_IOCTL_UPDATE  0x61   /* arg: uint32_t* (one word to feed) */
#define CRC_IOCTL_RESULT  0x62   /* arg: uint32_t* (current CRC) */

/*
 * Driver layer — generic STM32 CRC. Platform-independent: it keeps only the
 * ioctl surface and delegates ALL register work to the HAL. Implements the
 * unified `device` interface as a CONTROL-class device (a checksum engine,
 * parameter/state surface).
 */
typedef struct _crc crc;

struct _crc {
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    crc_hal_handle_t *hal;
};

device *crc_create(const void *config);
void crc_destroy(crc *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; crc_create() reads it. */
typedef struct {
    const char *name;    /* logical device name */
    void *periph;        /* CRC peripheral base (e.g. (void *)CRC) */
} crc_config_t;

#endif /* CRC_H */
