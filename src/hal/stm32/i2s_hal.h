#ifndef I2S_HAL_H
#define I2S_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — I2S (STM32F4).
 *
 * On the STM32F4 the I2S function is HOSTED inside the SPI peripheral: the same
 * register block (SPI_TypeDef) serves SPI when I2SCFGR.I2SMOD=0 and I2S when
 * I2SMOD=1. This HAL operates on a SPI_TypeDef* and switches it to I2S mode.
 *
 * The I2S bit clock is generated from the dedicated PLLI2S (RCC_PLLI2SCFGR) and
 * divided by the I2S prescaler (I2SPR) — it is INDEPENDENT of PCLK1/PCLK2. The
 * peripheral logic (FIFO + TXE flag) is clocked by that I2S clock, so PLLI2S
 * MUST be running before the I2S is enabled, otherwise TXE would never assert.
 *
 * All register knowledge stays here; the driver only sees the opaque handle.
 */
typedef struct i2s_hal_handle i2s_hal_handle_t;

i2s_hal_handle_t *i2s_hal_create(void *peripheral);
void i2s_hal_destroy(i2s_hal_handle_t *h);

/* Enable the SPI/I2S peripheral clock (APB1 for SPI2/3, APB2 for SPI1). */
void i2s_hal_enable_clock(i2s_hal_handle_t *h);

/* Configure + START the dedicated PLLI2S clock and return its output frequency
 * (I2SxCLK) in Hz. plli2sn = VCO multiplier, plli2sr = post-divider. The PLLI2S
 * input is HSE/PLLM (the same /M the main PLL uses). Stores the resulting
 * I2SxCLK in the handle so i2s_hal_config() can compute the prescaler. */
uint32_t i2s_hal_config_pll(i2s_hal_handle_t *h, uint32_t plli2sn, uint32_t plli2sr);

/* Configure the I2S for a given audio sample rate.
 *   std    : 0=Philips, 1=MSB-justified, 2=LSB-justified, 3=PCM
 *   datlen : 0=16-bit, 1=24-bit, 2=32-bit
 *   audio_hz : target sample rate (e.g. 48000)
 *   i2s_clk_hz : PLLI2S output (feed from i2s_hal_config_pll return)
 *   master : 1 = master (generates CK/WS), 0 = slave
 *   tx     : 1 = transmit, 0 = receive
 * Does NOT enable the I2S (I2SE stays 0) — call i2s_hal_enable() afterwards. */
void i2s_hal_config(i2s_hal_handle_t *h, uint32_t std, uint32_t datlen,
                    uint32_t audio_hz, uint32_t i2s_clk_hz, int master, int tx);

/* Enable/disable the I2S (I2SCFGR.I2SE). */
void i2s_hal_enable(i2s_hal_handle_t *h, int on);

/* Transmit one 16-bit sample (master TX). Waits for TXE with a timeout so a
 * missing PLLI2S clock can never hang the BIST. Returns 0 on success, -1 timeout. */
int i2s_hal_write_sample(i2s_hal_handle_t *h, uint16_t sample);

/* 1 if the transmit buffer can accept data (SR.TXE), else 0. */
int i2s_hal_tx_empty(i2s_hal_handle_t *h);

/* readback helpers for self-test verification */
uint32_t i2s_hal_get_i2scfgr(i2s_hal_handle_t *h);
uint32_t i2s_hal_get_i2spr(i2s_hal_handle_t *h);
uint32_t i2s_hal_get_plli2s(void);          /* RCC_PLLI2SCFGR */
uint32_t i2s_hal_pll_rdy(void);             /* RCC_CR.PLLI2SRDY */
uint32_t i2s_hal_get_cfgr(void);            /* RCC_CFGR (I2SSRC) */
uint32_t i2s_hal_get_i2s_clk_hz(i2s_hal_handle_t *h); /* PLLI2S output Hz (set by config_pll) */

#endif /* I2S_HAL_H */
