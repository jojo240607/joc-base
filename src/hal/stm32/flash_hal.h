#ifndef FLASH_HAL_H
#define FLASH_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — STM32F4 internal FLASH.
 *
 * The ONLY place that touches the FLASH registers. The driver stays
 * register-free. Implements erase / program / read of the on-chip flash so the
 * application can store data that survives power-down.
 *
 * SAFETY: the BIST erases and reprogrammes a SPARE sector (sector 7,
 * 0x08060000) that sits well above the firmware image (~57 KB, entirely within
 * sectors 0-3). Erasing/programming it can therefore never corrupt the running
 * code. The flash controller has no interrupt enabled, so the BSY busy-waits
 * here cannot deadlock (matching the IRQ/busy-wait rule in this codebase).
 */

typedef struct flash_hal_handle flash_hal_handle_t;

flash_hal_handle_t *flash_hal_create(void *peripheral);
void flash_hal_destroy(flash_hal_handle_t *h);

/* Base address of a flash sector (0..11) on the STM32F407 (1 MB). */
uint32_t flash_hal_sector_base(uint32_t sector);

/* Size in bytes of a flash sector (0..11) on the STM32F407 (1 MB). Sector 0-3
 * are 16 KB, sector 4 is 64 KB, sectors 5-11 are 128 KB. */
uint32_t flash_hal_sector_size(uint32_t sector);

/* Unlock the flash control register (KEYR sequence). Idempotent. */
void flash_hal_unlock(flash_hal_handle_t *h);

/* Erase a sector (destroys its contents). Waits for BSY to clear. */
void flash_hal_erase_sector(flash_hal_handle_t *h, uint32_t sector);

/* Program a 32-bit word at `addr` (must be 32-bit aligned, in an erased
 * sector). Waits for BSY to clear. */
void flash_hal_program_u32(flash_hal_handle_t *h, uint32_t addr, uint32_t value);

/* Read a 32-bit word, invalidating the D-cache line first so the value reflects
 * the physical flash (not a stale cached erase value). */
uint32_t flash_hal_read_u32(flash_hal_handle_t *h, uint32_t addr);

/* Raw FLASH status register (SR). */
uint32_t flash_hal_get_status(flash_hal_handle_t *h);

#endif /* FLASH_HAL_H */
