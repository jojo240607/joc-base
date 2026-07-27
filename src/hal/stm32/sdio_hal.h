#ifndef SDIO_HAL_H
#define SDIO_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — SDIO (STM32F4 SD/SDIO/MMC host interface).
 * All register knowledge stays here; the driver only sees the opaque handle.
 */
typedef struct sdio_hal_handle sdio_hal_handle_t;

sdio_hal_handle_t *sdio_hal_create(void *peripheral);
void sdio_hal_destroy(sdio_hal_handle_t *h);

void sdio_hal_enable_clock(sdio_hal_handle_t *h);
void sdio_hal_power_up(sdio_hal_handle_t *h);   /* POWER.PWRCTRL = 11 */
void sdio_hal_power_down(sdio_hal_handle_t *h);

/* Clock: SDIO_CK = 48 MHz / (CLKDIV + 2). Use clkdiv=0 for max (24 MHz). */
void sdio_hal_set_clock_div(sdio_hal_handle_t *h, uint32_t clkdiv);
void sdio_hal_enable_ck(sdio_hal_handle_t *h, int on);   /* CLKCR.CLKEN */
void sdio_hal_set_bus_width(sdio_hal_handle_t *h, int width); /* 1 or 4 */

/* Send a command and wait for response (polling, timeout-guarded).
 * resp_type: 0=no response, 1=short (48-bit, R1/R6/R7), 2=long (136-bit, R2).
 * resp: output buffer for (resp_type+1) uint32_t values.
 * Returns 0 on success, -1 on timeout/CRC error. */
int sdio_hal_cmd(sdio_hal_handle_t *h, uint32_t idx, uint32_t arg,
                 uint32_t resp_type, uint32_t *resp);

/* Configure and enable a data transfer (must precede cmd with data).
 * dir: 0=write (card→host), 1=read (host→card). blk_size in bytes.
 * count: number of blocks (0 = infinite for SDIO multi-byte). */
void sdio_hal_data_config(sdio_hal_handle_t *h, uint32_t dir,
                          uint32_t blk_size, uint32_t count);
void sdio_hal_data_enable(sdio_hal_handle_t *h, int on);  /* DCTRL.DTEN */

/* FIFO access (for data transfers). word_count must be blk_size/4. */
void sdio_hal_read_fifo(sdio_hal_handle_t *h, uint32_t *buf, int word_count);
void sdio_hal_write_fifo(sdio_hal_handle_t *h, const uint32_t *buf, int word_count);

/* Status */
uint32_t sdio_hal_get_sta(sdio_hal_handle_t *h);
void     sdio_hal_clear_icr(sdio_hal_handle_t *h, uint32_t mask);
/* Wait for the data phase to finish (DATAEND) or error (DTIMEOUT/DCRCFAIL).
 * Returns 0 on DATAEND, -1 on timeout/error (status cleared on error). Used by
 * the driver's DMA path to detect transfer completion. */
int      sdio_hal_wait_data_end(sdio_hal_handle_t *h, uint32_t timeout);

/* DMA data path: enable DMA on the data transfer (DCTRL.DMAEN) and expose the
 * FIFO address so the driver can configure its DMA stream. The SDIO host has a
 * SINGLE DMA request; direction is taken from `dir` (0=read P2M, 1=write M2P),
 * matching the DCTRL.DTDIR the driver already programmed. `sdio_hal_dma_enable`
 * toggles DMAEN independently (so it can be cleared after a transfer). */
void sdio_hal_data_config_dma(sdio_hal_handle_t *h, uint32_t dir,
                              uint32_t blk_size, uint32_t count);
void sdio_hal_dma_enable(sdio_hal_handle_t *h, int on);   /* DCTRL.DMAEN */
void *sdio_hal_get_fifo_addr(sdio_hal_handle_t *h);        /* (void *)&FIFO */
/* Clear the data-phase interrupt flags (DATAEND / DCRCFAIL / DTIMEOUT) after a
 * block transfer. Kept in the HAL so the driver never names chip constants. */
void sdio_hal_clear_data_icr(sdio_hal_handle_t *h);
/* Wait for ANY of mask bits in STA, timeout. Returns 1 on match, 0 on timeout. */
int      sdio_hal_wait_sta(sdio_hal_handle_t *h, uint32_t mask, uint32_t timeout);

/* Data transfer: wait for RX FIFO data, read/write block.
 * Returns 0 on success, -1 on error/timeout. */
int      sdio_hal_read_block(sdio_hal_handle_t *h, uint8_t *buf, uint32_t blk_addr,
                             uint32_t count, int is_sdhc);
int      sdio_hal_write_block(sdio_hal_handle_t *h, const uint8_t *buf, uint32_t blk_addr,
                              uint32_t count, int is_sdhc);
/* Send STOP (CMD12) to end multi-block transfer. */
int      sdio_hal_stop_transfer(sdio_hal_handle_t *h);

/* readback for verification */
uint32_t sdio_hal_get_power(sdio_hal_handle_t *h);
uint32_t sdio_hal_get_clkcr(sdio_hal_handle_t *h);

#endif /* SDIO_HAL_H */
