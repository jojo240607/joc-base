#ifndef I2C_HAL_H
#define I2C_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — I2C (MASTER mode, POLLING).
 *
 * NOTE ON REGISTER LAYOUT: the I2C peripheral on this silicon exposes the
 * F1-style register set (CR1/CR2/OAR1/OAR2/DR/SR1/SR2/CCR/TRISE) — verified
 * empirically on the board by probing register writability (16-bit registers,
 * CR1 bits 2/14 reserved, etc.), even though the part is an STM32F4. The
 * project's device/stm32f407xx.h already defines exactly this F1-style
 * I2C_TypeDef, so this HAL uses it directly (no local redefinition). All
 * register knowledge stays here; the driver only sees the opaque handle.
 *
 * The driver is POLLING (no IRQ): the F1 I2C has well-known errata around the
 * interrupt/state machine, and a polling loop with a hard timeout is the robust,
 * hang-free choice. Every wait is timeout-guarded.
 */
typedef struct i2c_hal_handle i2c_hal_handle_t;

i2c_hal_handle_t *i2c_hal_create(void *peripheral);
void i2c_hal_destroy(i2c_hal_handle_t *h);

void i2c_hal_enable_clock(i2c_hal_handle_t *h);
/* Program CCR/TRISE/CR2.FREQ for speed_hz (100000 or 400000). clk_hz = PCLK1
 * (42 MHz on F4). CR1.PE must be 0 to program CCR/TRISE, so we disable,
 * program, re-enable. */
void i2c_hal_config(i2c_hal_handle_t *h, uint32_t clk_hz, uint32_t speed_hz);
void i2c_hal_set_peripheral_enable(i2c_hal_handle_t *h, int on); /* CR1.PE */
void i2c_hal_software_reset(i2c_hal_handle_t *h);                 /* CR1.SWRST */

/* Master transfer (polling, timeout-guarded). Returns 0 on success, -1 on NACK
 * / timeout (no device / bus error). buf may be NULL for a zero-length transfer
 * (which still emits START + address + STOP — useful to probe a device). */
int i2c_hal_master_write(i2c_hal_handle_t *h, uint16_t addr,
                         const uint8_t *buf, uint16_t len);
int i2c_hal_master_read(i2c_hal_handle_t *h, uint16_t addr,
                        uint8_t *buf, uint16_t len);

/* readback helpers for self-test verification (F1 registers) */
uint32_t i2c_hal_get_ccr(i2c_hal_handle_t *h);       /* CCR[11:0] clock control */
uint32_t i2c_hal_get_cr2_freq(i2c_hal_handle_t *h); /* CR2.FREQ[5:0] (PCLK1 MHz) */
uint32_t i2c_hal_get_cr1(i2c_hal_handle_t *h);
int      i2c_hal_is_busy(i2c_hal_handle_t *h);

#endif /* I2C_HAL_H */
