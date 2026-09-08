#ifndef QSPI_HAL_H
#define QSPI_HAL_H

/*
 * Hardware Abstraction Layer — QUADSPI (STM32H7).
 *
 * Milestone 1: memory-mapped (XIP) mode so the 0x90000000 window exposes the
 * external flash holding the Rust app.
 * Milestone 2: indirect read / write / erase programming — the "QSPI 烧写
 * 接口" used to deploy/update the APP_FLASH partition on real silicon.
 *
 * Flash model: W25Q-class NOR (the STM32H750B-DK carries a 128-Mbit
 * quad-SPI NOR, W25Q128JV). All commands are 1-line SPI for maximum
 * compatibility; performance (16 MB at ~20 MHz) is secondary to correctness.
 *
 * Renode: no QUADSPI model exists, so indirect operations return -1 there
 * (qspi_hal_init_mm stays a no-op — the .resc loads the app image straight
 * into the 0x90000000 MappedMemory). Indirect programming is verified on
 * real hardware only.
 */

#include <stdint.h>

/* External flash geometry (W25Q128JV) */
#define QSPI_FLASH_SIZE     (16U * 1024U * 1024U)   /* 16 MB total */
#define QSPI_SECTOR_SIZE    4096U                   /* 4 KB sector  (0x20) */
#define QSPI_BLOCK_SIZE     65536U                  /* 64 KB block  (0xD8) */
#define QSPI_PAGE_SIZE      256U                    /* page program (0x02) */

/* Bring the QUADSPI controller up in memory-mapped mode so APP_FLASH
 * (0x90000000) is readable. Returns 0 on success.
 *
 * Under Renode (JOC_RENODE=1) this is a no-op: the .resc script loads the app
 * image directly into the 0x90000000 MappedMemory, so no register programming
 * is needed (the flash model is not configured). */
int qspi_hal_init_mm(void);

/* ---- indirect-mode programming API (milestone 2, real silicon only) ----
 * All functions return 0 on success, -1 on failure / unsupported (Renode).
 * They temporarily leave memory-mapped mode to run the command and restore
 * the previous CCR afterwards, so XIP stays functional between operations. */

/* Read the 3-byte JEDEC ID (0x9F): id[0]=manufacturer, id[1..2]=device. */
int qspi_hal_read_id(uint8_t id[3]);

/* Erase one 4 KB sector (0x20). addr must be QSPI_SECTOR_SIZE-aligned and
 * within QSPI_FLASH_SIZE. Blocks until the flash reports WIP=0. */
int qspi_hal_erase_sector(uint32_t addr);

/* Erase one 64 KB block (0xD8). addr must be QSPI_BLOCK_SIZE-aligned. */
int qspi_hal_erase_block(uint32_t addr);

/* Program one page (0x02): len in [1, QSPI_PAGE_SIZE], and addr must not
 * cross a 256-byte page boundary (caller splits multi-page writes). */
int qspi_hal_program(uint32_t addr, const uint8_t *data, uint32_t len);

/* Indirect read (0x03, 1-line): len bytes from addr into data. */
int qspi_hal_read(uint32_t addr, uint8_t *data, uint32_t len);

/* Poll flash WIP via RDSR (0x05) until write/erase completes or the loop
 * budget is exhausted. Returns 0 when idle, -1 on timeout. */
int qspi_hal_wait_ready(void);

#endif /* QSPI_HAL_H */
