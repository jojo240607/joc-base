#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include "iface/block_device.h"
#include "iface/device.h"
#include "hal/stm32h7/qspi_hal.h"
#include <stdint.h>

/*
 * External QUADSPI FLASH driver — a BLOCK device over the W25Q128 NOR that
 * holds the APP_FLASH partition (0x90000000 XIP window on STM32H750).
 *
 * Block granularity is one 256-byte page (block_size = 256), matching the
 * flash page-program unit. Reads use the QUADSPI indirect-read path; writes
 * split arbitrary lengths into page-sized program calls; erase is always
 * sector-granular (4 KB), as the hardware has no smaller erase unit.
 *
 * SAFETY: the driver only programs the external flash (never the running
 * internal flash image). BUSY-waits are loop-based (HAL has no interrupt
 * enabled), so they cannot deadlock with the RTOS. The driver never starts
 * any watchdog.
 *
 * Availability: real silicon only. Renode has no QUADSPI model, so every
 * operation fails with -1 there (see qspi_hal.h).
 */

typedef struct {
    const char *name;       /* logical device name (e.g. "qspi0") */
} qspi_flash_config_t;

typedef struct _qspi_flash qspi_flash;

struct _qspi_flash {
    block_device parent;            /* IS-A block device */
};

device *qspi_flash_create(const void *config);
void qspi_flash_destroy(qspi_flash *self);

/* IOCTL commands (driver-specific control via the base device vtable). */
#define QSPI_FLASH_IOCTL_GET_ID     0x80   /* uint8_t (*)[3] -> JEDEC ID */

#endif /* QSPI_FLASH_H */
