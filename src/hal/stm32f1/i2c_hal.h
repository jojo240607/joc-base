#ifndef I2C_HAL_H
#define I2C_HAL_H

#include <stdint.h>

/* irq_id_t forward-declared here to avoid pulling irq.h into board.c. */
typedef int irq_id_t;

/*
 * Hardware Abstraction Layer — I2C (STM32F1).
 *
 * F1 I2C peripheral uses the same F1-style register set as the F4 on this
 * board (CR1/CR2/OAR1/OAR2/DR/SR1/SR2/CCR/TRISE). All register knowledge
 * stays here; the driver only sees the opaque handle.
 *
 * The driver is POLLING (no IRQ): the F1 I2C has well-known errata around the
 * interrupt/state machine, and a polling loop with a hard timeout is the robust
 * choice. Every wait is timeout-guarded.
 */
typedef struct i2c_hal_handle i2c_hal_handle_t;

i2c_hal_handle_t *i2c_hal_create(void *peripheral);
void i2c_hal_destroy(i2c_hal_handle_t *h);

void i2c_hal_enable_clock(i2c_hal_handle_t *h);
void i2c_hal_config(i2c_hal_handle_t *h, uint32_t clk_hz, uint32_t speed_hz);
void i2c_hal_set_peripheral_enable(i2c_hal_handle_t *h, int on);
void i2c_hal_software_reset(i2c_hal_handle_t *h);

int i2c_hal_master_write(i2c_hal_handle_t *h, uint16_t addr,
                         const uint8_t *buf, uint16_t len);
int i2c_hal_master_read(i2c_hal_handle_t *h, uint16_t addr,
                        uint8_t *buf, uint16_t len);
int i2c_hal_master_start_addr(i2c_hal_handle_t *h, uint16_t addr, int is_write);

/* --- Interrupt mode helpers --- */
void     i2c_hal_ev_isr(void *ctx);
void     i2c_hal_er_isr(void *ctx);
irq_id_t i2c_hal_ev_irq_id(i2c_hal_handle_t *h);
irq_id_t i2c_hal_er_irq_id(i2c_hal_handle_t *h);
void     i2c_hal_enable_ev_irq(i2c_hal_handle_t *h);
void     i2c_hal_disable_ev_irq(i2c_hal_handle_t *h);
void     i2c_hal_enable_er_irq(i2c_hal_handle_t *h);
void     i2c_hal_disable_er_irq(i2c_hal_handle_t *h);
uint32_t i2c_hal_read_sr1(i2c_hal_handle_t *h);
uint32_t i2c_hal_read_sr2(i2c_hal_handle_t *h);
void     i2c_hal_write_dr(i2c_hal_handle_t *h, uint8_t data);
uint8_t  i2c_hal_read_dr(i2c_hal_handle_t *h);
void     i2c_hal_set_start(i2c_hal_handle_t *h);
void     i2c_hal_set_stop(i2c_hal_handle_t *h);
void     i2c_hal_set_ack(i2c_hal_handle_t *h, int on);
void     i2c_hal_set_pos(i2c_hal_handle_t *h, int on);
void     i2c_hal_clear_sr1_af(i2c_hal_handle_t *h);
void     i2c_hal_nvic_enable(int irq);
void     i2c_hal_nvic_disable(int irq);

/* --- DMA mode helpers --- */
void     i2c_hal_dma_enable(i2c_hal_handle_t *h, int on);
void     i2c_hal_set_dma_last(i2c_hal_handle_t *h, int on);
void    *i2c_hal_get_dr_addr(i2c_hal_handle_t *h);
int      i2c_hal_wait_btf(i2c_hal_handle_t *h, uint32_t timeout);

/* readback helpers for self-test verification */
uint32_t i2c_hal_get_ccr(i2c_hal_handle_t *h);
uint32_t i2c_hal_get_cr2_freq(i2c_hal_handle_t *h);
uint32_t i2c_hal_get_cr1(i2c_hal_handle_t *h);
int      i2c_hal_is_busy(i2c_hal_handle_t *h);

#endif /* I2C_HAL_H */