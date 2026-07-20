#include "i2c_hal.h"
#include "stm32f4xx.h"     /* I2C_TypeDef (F1-style — this silicon uses the F1 I2C
                             * peripheral), RCC, I2C1/2/3 base addresses + bit defs */
#include <stdlib.h>

/* Per-wait timeout (tight-loop iterations). At 168 MHz a few hundred k iters is
 * a couple ms — long enough for a slow 100 kHz byte, short enough to never hang. */
#define I2C_WAIT_TIMEOUT  200000U

/*
 * Hardware Abstraction Layer — I2C (MASTER mode, POLLING).
 *
 * NOTE ON REGISTER LAYOUT: although STM32F4 is often described with the "new"
 * I2C (TIMINGR/ISR/ICR/TXDR/RXDR), the I2C peripheral on THIS silicon exposes
 * the F1-style register set (CR1/CR2/OAR1/OAR2/DR/SR1/SR2/CCR/TRISE) — verified
 * empirically by probing register writability on the board (16-bit registers,
 * CR1 bits 2/14 reserved, etc.). The project's device/stm32f407xx.h already
 * defines exactly this F1-style I2C_TypeDef, so we use it directly. All register
 * knowledge stays here; the driver only sees the opaque handle.
 *
 * The driver is POLLING (no IRQ): the F1 I2C has well-known errata around the
 * interrupt/state machine, and a polling loop with a hard timeout is the robust,
 * hang-free choice. Every wait is timeout-guarded so a stuck/floating bus can
 * never wedge the CPU.
 */

struct i2c_hal_handle {
    I2C_TypeDef *reg;       /* F1-style I2C peripheral */
    uint32_t  apb1_en;      /* RCC_APB1ENR bit to gate this I2C's clock */
};

i2c_hal_handle_t *i2c_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    i2c_hal_handle_t *h = (i2c_hal_handle_t *)malloc(sizeof(i2c_hal_handle_t));
    if (!h) return NULL;
    h->reg = (I2C_TypeDef *)peripheral;
    /* pick the APB1 clock-enable bit from the base address. */
    if (peripheral == (void *)I2C1_BASE)      h->apb1_en = RCC_APB1ENR_I2C1EN;
    else if (peripheral == (void *)I2C2_BASE) h->apb1_en = RCC_APB1ENR_I2C2EN;
    else if (peripheral == (void *)I2C3_BASE) h->apb1_en = RCC_APB1ENR_I2C3EN;
    else                                      h->apb1_en = RCC_APB1ENR_I2C1EN; /* default */
    return h;
}

void i2c_hal_destroy(i2c_hal_handle_t *h) { free(h); }

void i2c_hal_enable_clock(i2c_hal_handle_t *h)
{
    if (!h) return;
    RCC->APB1ENR |= h->apb1_en;
}

/* Program the F1 I2C clock for speed_hz (standard mode, 50% duty). clk_hz is
 * PCLK1 (42 MHz on F4). CR1.PE must be 0 to program CCR/TRISE, so we disable,
 * program, re-enable. */
void i2c_hal_config(i2c_hal_handle_t *h, uint32_t clk_hz, uint32_t speed_hz)
{
    if (!h) return;
    I2C_TypeDef *r = h->reg;
    uint32_t freq_mhz = clk_hz / 1000000UL;          /* PCLK1 in MHz (42) */

    r->CR1 &= ~I2C_CR1_PE;
    /* CR2.FREQ = peripheral clock in MHz (used by the hardware for timing). */
    r->CR2 = (r->CR2 & ~I2C_CR2_FREQ) | (freq_mhz & I2C_CR2_FREQ);
    /* CCR: standard mode (FS=0), duty 50% (DUTY=0): CCR = PCLK1 / (2 * speed). */
    uint32_t ccr = clk_hz / (2UL * speed_hz);
    if (ccr < 4UL) ccr = 4UL;                         /* CCR floor per datasheet */
    r->CCR = ccr & I2C_CCR_CCR;
    /* TRISE = (max rise time / TPCLK1) + 1 = freq_mhz + 1 (1000 ns / (1/freq_mhz)). */
    r->TRISE = (freq_mhz + 1UL) & I2C_TRISE_TRISE;
    r->CR1 |= I2C_CR1_PE;
}

void i2c_hal_set_peripheral_enable(i2c_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->CR1 |= I2C_CR1_PE;
    else    h->reg->CR1 &= ~I2C_CR1_PE;
}

void i2c_hal_software_reset(i2c_hal_handle_t *h)
{
    if (!h) return;
    I2C_TypeDef *r = h->reg;
    r->CR1 |= I2C_CR1_SWRST;
    r->CR1 &= ~I2C_CR1_SWRST;
}

/* Wait until ANY bit in `mask` is set in SR1, or the timeout expires. */
static int i2c_wait_sr1(I2C_TypeDef *r, uint32_t mask)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) {
        if (r->SR1 & mask) return 1;
    }
    return 0;
}

/* Wait until the bus is free (SR2.BUSY cleared). Returns 1 if free, 0 on timeout. */
static int i2c_wait_bus_free(I2C_TypeDef *r)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) {
        if (!(r->SR2 & I2C_SR2_BUSY)) return 1;
    }
    return 0;
}

int i2c_hal_master_write(i2c_hal_handle_t *h, uint16_t addr,
                          const uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;

    if (!i2c_wait_bus_free(r)) return -1;            /* stuck bus -> give up */
    r->CR1 |= I2C_CR1_START;                          /* generate START */
    if (!i2c_wait_sr1(r, I2C_SR1_SB)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->DR = (uint8_t)((addr << 1) & 0xFEU);           /* address + W (bit0=0) */

    /* wait for ADDR (acked) or AF (nacked) */
    if (!i2c_wait_sr1(r, I2C_SR1_ADDR | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) {                           /* no such device */
        r->CR1 |= I2C_CR1_STOP; return -1;
    }
    (void)r->SR2;                                       /* clear ADDR */

    if (len == 0) { r->CR1 |= I2C_CR1_STOP; return 0; } /* probe: ACK + STOP */

    for (uint16_t i = 0; i < len; i++) {
        if (!i2c_wait_sr1(r, I2C_SR1_TXE | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
        r->DR = buf[i];
    }
    /* wait for the last byte to finish shifting (BTF), then STOP */
    if (!i2c_wait_sr1(r, I2C_SR1_BTF | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->CR1 |= I2C_CR1_STOP;
    return 0;
}

int i2c_hal_master_read(i2c_hal_handle_t *h, uint16_t addr,
                         uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;

    if (!i2c_wait_bus_free(r)) return -1;
    r->CR1 |= I2C_CR1_START;                          /* generate START */
    if (!i2c_wait_sr1(r, I2C_SR1_SB)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->DR = (uint8_t)((addr << 1) | 0x01U);           /* address + R (bit0=1) */

    if (!i2c_wait_sr1(r, I2C_SR1_ADDR | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }

    if (len == 0) {                                   /* probe read: ACK, then STOP */
        (void)r->SR2; r->CR1 |= I2C_CR1_STOP; return 0;
    }
    if (len == 1) {                                   /* single byte: NACK + STOP */
        r->CR1 &= ~I2C_CR1_ACK;
        r->CR1 |= I2C_CR1_STOP;
        (void)r->SR2;                                 /* clear ADDR */
        if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        buf[0] = (uint8_t)r->DR;
        return 0;
    }
    if (len == 2) {                                   /* two bytes: POS + NACK */
        r->CR1 |= I2C_CR1_POS;
        (void)r->SR2;                                 /* clear ADDR */
        r->CR1 &= ~I2C_CR1_ACK;
        if (!i2c_wait_sr1(r, I2C_SR1_BTF)) { r->CR1 &= ~I2C_CR1_POS; r->CR1 |= I2C_CR1_STOP; return -1; }
        r->CR1 |= I2C_CR1_STOP;
        buf[0] = (uint8_t)r->DR;
        buf[1] = (uint8_t)r->DR;
        r->CR1 &= ~I2C_CR1_POS;
        return 0;
    }
    /* len >= 3: receive N-2 bytes with ACK, then NACK the last byte */
    (void)r->SR2;                                     /* clear ADDR */
    r->CR1 |= I2C_CR1_ACK;
    for (uint16_t i = 0; i < (uint16_t)(len - 2); i++) {
        if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        buf[i] = (uint8_t)r->DR;
    }
    r->CR1 &= ~I2C_CR1_ACK;                           /* NACK the final byte */
    if (!i2c_wait_sr1(r, I2C_SR1_BTF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    buf[len - 2] = (uint8_t)r->DR;                    /* read N-2 */
    if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->CR1 |= I2C_CR1_STOP;                           /* STOP before the last read */
    buf[len - 1] = (uint8_t)r->DR;                    /* read N-1 */
    return 0;
}

/* readback helpers for self-test verification (F1 registers) */
uint32_t i2c_hal_get_ccr(i2c_hal_handle_t *h)
    { return h ? (h->reg->CCR & I2C_CCR_CCR) : 0UL; }
uint32_t i2c_hal_get_cr2_freq(i2c_hal_handle_t *h)
    { return h ? (h->reg->CR2 & I2C_CR2_FREQ) : 0UL; }
uint32_t i2c_hal_get_cr1(i2c_hal_handle_t *h)
    { return h ? h->reg->CR1 : 0UL; }
int i2c_hal_is_busy(i2c_hal_handle_t *h)
    { return h ? ((h->reg->SR2 & I2C_SR2_BUSY) ? 1 : 0) : 0; }
