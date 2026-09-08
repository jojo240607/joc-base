/*
 * I2C hardware abstraction layer for STM32F103.
 *
 * The F1 I2C peripheral uses the exact same register layout as the F1-style
 * I2C on the F4 board (CR1/CR2/OAR1/OAR2/DR/SR1/SR2/CCR/TRISE). This
 * implementation is adapted from the stm32/ (F4) version, with stm32f1xx.h
 * includes and F1-specific base addresses and clock-enable bits.
 *
 * F103 has I2C1 and I2C2 (no I2C3).
 */
#include "i2c_hal.h"
#include "stm32f1xx.h"     /* stm32f103xx.h I2C_TypeDef + RCC + base defs */
#include <stdlib.h>

#define I2C_WAIT_TIMEOUT  200000U

struct i2c_hal_handle {
    I2C_TypeDef *reg;
    uint32_t  apb1_en;
};

i2c_hal_handle_t *i2c_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    i2c_hal_handle_t *h = (i2c_hal_handle_t *)malloc(sizeof(i2c_hal_handle_t));
    if (!h) return NULL;
    h->reg = (I2C_TypeDef *)peripheral;
    if      (peripheral == (void *)I2C1_BASE) h->apb1_en = RCC_APB1ENR_I2C1EN;
    else if (peripheral == (void *)I2C2_BASE) h->apb1_en = RCC_APB1ENR_I2C2EN;
    else                                       h->apb1_en = RCC_APB1ENR_I2C1EN;
    return h;
}

void i2c_hal_destroy(i2c_hal_handle_t *h) { free(h); }

void i2c_hal_enable_clock(i2c_hal_handle_t *h)
{
    if (!h) return;
    RCC->APB1ENR |= h->apb1_en;
}

void i2c_hal_config(i2c_hal_handle_t *h, uint32_t clk_hz, uint32_t speed_hz)
{
    if (!h) return;
    I2C_TypeDef *r = h->reg;
    uint32_t freq_mhz = clk_hz / 1000000UL;
    r->CR1 &= ~I2C_CR1_PE;
    r->CR2 = (r->CR2 & ~I2C_CR2_FREQ) | (freq_mhz & I2C_CR2_FREQ);
    uint32_t ccr = clk_hz / (2UL * speed_hz);
    if (ccr < 4UL) ccr = 4UL;
    r->CCR = ccr & I2C_CCR_CCR;
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

static int i2c_wait_sr1(I2C_TypeDef *r, uint32_t mask)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) { if (r->SR1 & mask) return 1; }
    return 0;
}

static int i2c_wait_bus_free(I2C_TypeDef *r)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) { if (!(r->SR2 & I2C_SR2_BUSY)) return 1; }
    return 0;
}

int i2c_hal_master_write(i2c_hal_handle_t *h, uint16_t addr,
                          const uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;
    if (!i2c_wait_bus_free(r)) return -1;
    r->CR1 |= I2C_CR1_START;
    if (!i2c_wait_sr1(r, I2C_SR1_SB)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->DR = (uint8_t)((addr << 1) & 0xFEU);
    if (!i2c_wait_sr1(r, I2C_SR1_ADDR | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
    (void)r->SR2;
    if (len == 0) { r->CR1 |= I2C_CR1_STOP; return 0; }
    for (uint16_t i = 0; i < len; i++) {
        if (!i2c_wait_sr1(r, I2C_SR1_TXE | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
        r->DR = buf[i];
    }
    if (!i2c_wait_sr1(r, I2C_SR1_BTF | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->CR1 |= I2C_CR1_STOP;
    return 0;
}

int i2c_hal_master_start_addr(i2c_hal_handle_t *h, uint16_t addr, int is_write)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;
    if (!i2c_wait_bus_free(r)) return -1;
    r->CR1 |= I2C_CR1_START;
    if (!i2c_wait_sr1(r, I2C_SR1_SB)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->DR = (uint8_t)((addr << 1) | (is_write ? 0U : 1U));
    if (!i2c_wait_sr1(r, I2C_SR1_ADDR | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
    return 0;
}

int i2c_hal_master_read(i2c_hal_handle_t *h, uint16_t addr,
                        uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;
    if (!i2c_wait_bus_free(r)) return -1;
    r->CR1 |= I2C_CR1_START;
    if (!i2c_wait_sr1(r, I2C_SR1_SB)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->DR = (uint8_t)((addr << 1) | 0x01U);
    if (!i2c_wait_sr1(r, I2C_SR1_ADDR | I2C_SR1_AF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (r->SR1 & I2C_SR1_AF) { r->CR1 |= I2C_CR1_STOP; return -1; }
    if (len == 0) { (void)r->SR2; r->CR1 |= I2C_CR1_STOP; return 0; }
    if (len == 1) {
        r->CR1 &= ~I2C_CR1_ACK; r->CR1 |= I2C_CR1_STOP;
        (void)r->SR2;
        if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        buf[0] = (uint8_t)r->DR; return 0;
    }
    if (len == 2) {
        r->CR1 |= I2C_CR1_POS; (void)r->SR2; r->CR1 &= ~I2C_CR1_ACK;
        if (!i2c_wait_sr1(r, I2C_SR1_BTF)) { r->CR1 &= ~I2C_CR1_POS; r->CR1 |= I2C_CR1_STOP; return -1; }
        r->CR1 |= I2C_CR1_STOP; buf[0] = (uint8_t)r->DR; buf[1] = (uint8_t)r->DR;
        r->CR1 &= ~I2C_CR1_POS; return 0;
    }
    (void)r->SR2; r->CR1 |= I2C_CR1_ACK;
    for (uint16_t i = 0; i < (uint16_t)(len - 2); i++) {
        if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
        buf[i] = (uint8_t)r->DR;
    }
    r->CR1 &= ~I2C_CR1_ACK;
    if (!i2c_wait_sr1(r, I2C_SR1_BTF)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    buf[len - 2] = (uint8_t)r->DR;
    if (!i2c_wait_sr1(r, I2C_SR1_RXNE)) { r->CR1 |= I2C_CR1_STOP; return -1; }
    r->CR1 |= I2C_CR1_STOP;
    buf[len - 1] = (uint8_t)r->DR;
    return 0;
}

uint32_t i2c_hal_get_ccr(i2c_hal_handle_t *h)
    { return h ? (h->reg->CCR & I2C_CCR_CCR) : 0UL; }
uint32_t i2c_hal_get_cr2_freq(i2c_hal_handle_t *h)
    { return h ? (h->reg->CR2 & I2C_CR2_FREQ) : 0UL; }
uint32_t i2c_hal_get_cr1(i2c_hal_handle_t *h)
    { return h ? h->reg->CR1 : 0UL; }
int i2c_hal_is_busy(i2c_hal_handle_t *h)
    { return h ? ((h->reg->SR2 & I2C_SR2_BUSY) ? 1 : 0) : 0; }

irq_id_t i2c_hal_ev_irq_id(i2c_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)I2C1_BASE) return (irq_id_t)I2C1_EV_IRQn;
    else if (p == (void *)I2C2_BASE) return (irq_id_t)I2C2_EV_IRQn;
    return -1;
}
irq_id_t i2c_hal_er_irq_id(i2c_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)I2C1_BASE) return (irq_id_t)I2C1_ER_IRQn;
    else if (p == (void *)I2C2_BASE) return (irq_id_t)I2C2_ER_IRQn;
    return -1;
}
void i2c_hal_enable_ev_irq(i2c_hal_handle_t *h)   { if (h) h->reg->CR2 |= I2C_CR2_ITEVTEN; }
void i2c_hal_disable_ev_irq(i2c_hal_handle_t *h)  { if (h) h->reg->CR2 &= ~I2C_CR2_ITEVTEN; }
void i2c_hal_enable_er_irq(i2c_hal_handle_t *h)   { if (h) h->reg->CR2 |= I2C_CR2_ITERREN; }
void i2c_hal_disable_er_irq(i2c_hal_handle_t *h)  { if (h) h->reg->CR2 &= ~I2C_CR2_ITERREN; }
uint32_t i2c_hal_read_sr1(i2c_hal_handle_t *h)    { return h ? h->reg->SR1 : 0UL; }
uint32_t i2c_hal_read_sr2(i2c_hal_handle_t *h)    { return h ? h->reg->SR2 : 0UL; }
void i2c_hal_write_dr(i2c_hal_handle_t *h, uint8_t data) { if (h) h->reg->DR = data; }
uint8_t i2c_hal_read_dr(i2c_hal_handle_t *h)      { return h ? (uint8_t)h->reg->DR : 0U; }
void i2c_hal_set_start(i2c_hal_handle_t *h)       { if (h) h->reg->CR1 |= I2C_CR1_START; }
void i2c_hal_set_stop(i2c_hal_handle_t *h)        { if (h) h->reg->CR1 |= I2C_CR1_STOP; }
void i2c_hal_set_ack(i2c_hal_handle_t *h, int on) { if (h) { if (on) h->reg->CR1 |= I2C_CR1_ACK; else h->reg->CR1 &= ~I2C_CR1_ACK; } }
void i2c_hal_set_pos(i2c_hal_handle_t *h, int on) { if (h) { if (on) h->reg->CR1 |= I2C_CR1_POS; else h->reg->CR1 &= ~I2C_CR1_POS; } }
void i2c_hal_clear_sr1_af(i2c_hal_handle_t *h)    { if (h) { (void)h->reg->SR1; h->reg->CR1 |= I2C_CR1_STOP; } }
void i2c_hal_nvic_enable(int irq)                  { if (irq >= 0) NVIC_EnableIRQ((IRQn_Type)irq); }
void i2c_hal_nvic_disable(int irq)                 { if (irq >= 0) NVIC_DisableIRQ((IRQn_Type)irq); }

void i2c_hal_dma_enable(i2c_hal_handle_t *h, int on)
    { if (h) { if (on) h->reg->CR2 |= I2C_CR2_DMAEN; else h->reg->CR2 &= ~I2C_CR2_DMAEN; } }
void i2c_hal_set_dma_last(i2c_hal_handle_t *h, int on)
    { if (h) { if (on) h->reg->CR2 |= I2C_CR2_LAST; else h->reg->CR2 &= ~I2C_CR2_LAST; } }
void *i2c_hal_get_dr_addr(i2c_hal_handle_t *h)      { return h ? (void *)&h->reg->DR : NULL; }

int i2c_hal_wait_btf(i2c_hal_handle_t *h, uint32_t timeout)
{
    if (!h) return 0;
    volatile uint32_t n = timeout;
    while (n--) { if (h->reg->SR1 & I2C_SR1_BTF) return 1; }
    return 0;
}

/* =========================================================================
 * I2C ISR state machine
 * ========================================================================= */

#include "drv/i2c.h"

#define I2CS_SB   (1U << 0)
#define I2CS_ADDR (1U << 1)
#define I2CS_BTF  (1U << 2)
#define I2CS_RXNE (1U << 6)
#define I2CS_TXE  (1U << 7)
#define I2CS_AF   (1U << 10)

#define I2C_S_IDLE 0
#define I2C_S_SB   1
#define I2C_S_ADDR 2
#define I2C_S_DATA 3
#define I2C_S_BTF  4
#define I2C_S_DONE 5

void i2c_hal_ev_isr(void *ctx)
{
    i2c *p = (i2c *)ctx;
    if (p->parent.mode != STREAM_MODE_IRQ) return;
    i2c_irq_t *e = (i2c_irq_t *)p->eng;
    if (!e) return;
    uint32_t sr1 = i2c_hal_read_sr1(p->hal);
    int is_write = (e->tx_buf != NULL);

    if ((sr1 & I2CS_SB) && e->irq_state == I2C_S_SB) {
        i2c_hal_write_dr(p->hal, (uint8_t)((e->addr << 1) | (is_write ? 0U : 1U)));
        e->irq_state = I2C_S_ADDR; return;
    }
    if ((sr1 & I2CS_ADDR) && e->irq_state == I2C_S_ADDR) {
        if (!is_write) {
            uint16_t l = e->xfer_len;
            if (l == 0) { (void)i2c_hal_read_sr2(p->hal); i2c_hal_set_stop(p->hal); e->irq_result = 0; e->irq_state = I2C_S_DONE; e->xfer_done = 1; return; }
            if (l == 1) { i2c_hal_set_ack(p->hal, 0); i2c_hal_set_stop(p->hal); }
            else if (l == 2) i2c_hal_set_pos(p->hal, 1);
        }
        (void)i2c_hal_read_sr2(p->hal);
        e->xfer_pos = 0; e->irq_state = I2C_S_DATA; return;
    }
    if ((sr1 & I2CS_TXE) && is_write && e->irq_state == I2C_S_DATA) {
        uint16_t pos = e->xfer_pos;
        if (pos < e->xfer_len) { i2c_hal_write_dr(p->hal, e->tx_buf[pos]); e->xfer_pos = pos + 1; }
        if (pos + 1 >= e->xfer_len) e->irq_state = I2C_S_BTF;
        return;
    }
    if ((sr1 & I2CS_BTF) && e->irq_state == I2C_S_BTF) {
        if (!is_write) {
            if (e->xfer_len == 2) {
                uint8_t b0 = i2c_hal_read_dr(p->hal), b1 = i2c_hal_read_dr(p->hal);
                if (e->rx_buf) { e->rx_buf[0] = b0; e->rx_buf[1] = b1; }
                i2c_hal_set_stop(p->hal); i2c_hal_set_pos(p->hal, 0);
                e->irq_result = 0; e->irq_state = I2C_S_DONE; e->xfer_done = 1; return;
            }
            uint16_t pos = e->xfer_pos;
            if (pos < e->xfer_len && e->rx_buf) e->rx_buf[pos] = i2c_hal_read_dr(p->hal);
            e->xfer_pos = pos + 1; e->irq_state = I2C_S_DATA; return;
        }
        i2c_hal_set_stop(p->hal);
        e->irq_result = 0; e->irq_state = I2C_S_DONE; e->xfer_done = 1; return;
    }
    if ((sr1 & I2CS_RXNE) && !is_write && e->irq_state == I2C_S_DATA) {
        uint16_t pos = e->xfer_pos;
        uint8_t d = i2c_hal_read_dr(p->hal);
        if (pos < e->xfer_len && e->rx_buf) e->rx_buf[pos] = d;
        pos++; e->xfer_pos = pos;
        if (e->xfer_len == 1) { e->irq_result = 0; e->irq_state = I2C_S_DONE; e->xfer_done = 1; return; }
        if (e->xfer_len >= 3 && pos >= e->xfer_len - 1) { e->irq_result = 0; e->irq_state = I2C_S_DONE; e->xfer_done = 1; return; }
        if (e->xfer_len >= 3 && pos == e->xfer_len - 2) { i2c_hal_set_ack(p->hal, 0); e->irq_state = I2C_S_BTF; }
        return;
    }
}
void i2c_hal_er_isr(void *ctx)
{
    i2c *p = (i2c *)ctx;
    if (p->parent.mode != STREAM_MODE_IRQ) return;
    i2c_irq_t *e = (i2c_irq_t *)p->eng;
    if (!e) return;
    if (i2c_hal_read_sr1(p->hal) & I2CS_AF) {
        (void)i2c_hal_read_sr1(p->hal); i2c_hal_set_stop(p->hal);
        e->irq_result = -1; e->irq_state = I2C_S_DONE; e->xfer_done = 1;
    }
}