#ifndef FLASH_H
#define FLASH_H

#include "iface/block_device.h"
#include "iface/device.h"
#include "hal/stm32/flash_hal.h"   /* flash_hal_handle_t (HAL public interface) */
#include <stdint.h>

/*
 * Internal FLASH driver — a BLOCK device over ONE on-chip flash SECTOR.
 *
 * The driver manages a single flash sector (chosen by the board) and exposes
 * it through the block_device interface, where a "block" is one 32-bit word
 * (block_size = 4). Read / write go word-by-word through flash_hal; erase is
 * ALWAYS sector-granular (the hardware has no smaller erase unit), so the
 * block erase operation erases the whole managed sector regardless of the
 * lba/count arguments.
 *
 * SAFETY: the board must point this driver at a SPARE sector the firmware
 * never occupies — sector 11 (0x080E0000, 128 KB) on the STM32F407 sits well
 * above the ~57 KB image (sectors 0-3) AND above the APP_FLASH app partition
 * (sectors 7/8/9), so erasing/programming it can never corrupt the running code
 * or the app. The flash controller has no interrupt enabled, so
 * the BSY busy-waits in flash_hal cannot deadlock (matching the IRQ/busy-wait
 * rule in this codebase). The driver never starts any watchdog, so it can
 * never brick the board.
 */

typedef struct {
    const char *name;       /* logical device name (e.g. "flash0") */
    uint32_t sector;        /* flash sector index to manage (0..11 on F407) */
} flash_config_t;

typedef struct _flash flash;

struct _flash {
    block_device parent;            /* IS-A block device */
    flash_hal_handle_t *hal;        /* opaque HAL handle (no register access) */
    uint32_t sector;                /* managed sector index */
    uint32_t base;                  /* absolute sector base address */
    uint32_t sector_size;           /* managed sector size in bytes */
};

device *flash_create(const void *config);
void flash_destroy(flash *self);

/* IOCTL commands (driver-specific control via the base device vtable). */
#define FLASH_IOCTL_GET_SECTOR 0x70   /* uint32_t* -> managed sector index */
#define FLASH_IOCTL_GET_BASE   0x71   /* uint32_t* -> absolute sector base addr */
#define FLASH_IOCTL_GET_STATUS 0x72   /* uint32_t* -> raw FLASH->SR */

#endif /* FLASH_H */
