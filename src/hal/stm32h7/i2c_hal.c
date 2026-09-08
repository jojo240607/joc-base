#include "i2c_hal.h"
#include "stm32h750xx.h"   /* I2C_TypeDef (F7-style), RCC, I2Cx_BASE + bit defs */
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — I2C (STM32H7 F7-style, master, polling).
 *
 * H7 I2C is the F7-style peripheral (TIMINGR/ISR/ICR/TXDR/RXDR — NOT the
 * F1-style CR1/CR2/SR1/SR2/DR/CCR/TRISE). The driver API from i2c_hal.h
 * (shared across platforms) is implemented here with the F7 register set:
 *
 *   - Speed set via TIMINGR (not CCR/TRISE)
 *   - Status via ISR (not SR1/SR2)
 *   - Clear via ICR writes (not read-modify-read sequence)
 *   - NBYTES must be programmed before START
 *
 * Only POLLING mode is implemented for now. The DMA path helpers
 * (master_start_addr, DMAEN, etc.) are stubs that document the NBYTES
 * timing mismatch.
 */

/* Per-wait timeout (tight-loop iterations). At 480 MHz, a few hundred k
 * iterations is ~1 ms — sufficient for 100 kHz I2C. */
#define I2C_WAIT_TIMEOUT  200000U

/* I2C timing register values for 240 MHz PCLK1:
 * Computed for a 4 MHz I2CCLK (prescaler from RCC_D2CCIP2R.I2CxSEL=0 with
 * appropriate divider). For a robust port these should be board-calibrated;
 * the values below target 100 kHz (standard-mode) and 400 kHz (fast-mode)
 * with 50% duty, rise time ~1 us, fall time ~100 ns.
 *   PRESC[3:0]   = 0x0    (I2CCLK / 1 = 4 MHz)
 *   SCLDEL[3:0]  = 0x3    (data setup time ~3 I2CCLK cycles)
 *   SDADEL[3:0]  = 0x2    (data hold time ~2 I2CCLK cycles)
 *   SCLH[7:0]    = 0x13   (SCL high = 20 I2CCLK for 100 kHz with proper duty)
 *   SCLL[7:0]    = 0x1F   (SCL low  = 30 I2CCLK for 100 kHz with proper duty)
 * Fast-mode 400 kHz:
 *   SCLH = 0x06, SCLL = 0x0A
 */
#define I2C_TIMINGR_100KHZ  0x00100313UL
#define I2C_TIMINGR_400KHZ  0x00100506UL

struct i2c_hal_handle {
    I2C_TypeDef *reg;
    uint32_t     apb1_en;   /* RCC_APB1LENR bit for this I2C */
};

i2c_hal_handle_t *i2c_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    i2c_hal_handle_t *h = (i2c_hal_handle_t *)malloc(sizeof(i2c_hal_handle_t));
    if (!h) return NULL;
    h->reg = (I2C_TypeDef *)peripheral;
    if (peripheral == (void *)I2C1_BASE)      h->apb1_en = RCC_APB1LENR_I2C1EN;
    else if (peripheral == (void *)I2C2_BASE) h->apb1_en = RCC_APB1LENR_I2C2EN;
    else if (peripheral == (void *)I2C3_BASE) h->apb1_en = RCC_APB1LENR_I2C3EN;
    else if (peripheral == (void *)I2C4_BASE) h->apb1_en = 0UL;  /* I2C4 on APB4 */
    else                                      h->apb1_en = RCC_APB1LENR_I2C1EN;
    return h;
}

void i2c_hal_destroy(i2c_hal_handle_t *h) { free(h); }

void i2c_hal_enable_clock(i2c_hal_handle_t *h)
{
    if (!h) return;
    if (h->reg == (I2C_TypeDef *)I2C4_BASE) {
        RCC->APB4ENR |= RCC_APB4ENR_I2C4EN;
    } else {
        RCC->APB1LENR |= h->apb1_en;
    }
}

void i2c_hal_config(i2c_hal_handle_t *h, uint32_t clk_hz, uint32_t speed_hz)
{
    (void)clk_hz;
    if (!h) return;
    I2C_TypeDef *r = h->reg;

    r->CR1 &= ~I2C_CR1_PE;                      /* disable during config */

    r->TIMINGR = (speed_hz >= 400000UL) ? I2C_TIMINGR_400KHZ
                                        : I2C_TIMINGR_100KHZ;

    /* Own address 1: 7-bit, default 0x00 (board config can override) */
    r->OAR1 = (0x1UL << 15U);                   /* OA1EN = 1, OA1 mode = 7-bit */

    /* CR1: enable, no auto-end (NBYTES will be set before START with AUTOEND=0
     * so the driver or caller issues STOP). */
    r->CR1 = I2C_CR1_PE;
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
    r->CR1 &= ~I2C_CR1_PE;                      /* disable PE */
    /* F7 I2C: no SWRST bit — just disable/enable */
    r->CR1 |= I2C_CR1_PE;
}

/* ------------------------------------------------------------------ */
/*  Internal helpers (static)                                         */
/* ------------------------------------------------------------------ */

/* Wait until ANY bit in `mask` is set in ISR, or timeout. Returns 1 if
 * a mask bit was set, 0 on timeout. */
static int i2c_wait_isr(I2C_TypeDef *r, uint32_t mask)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) {
        uint32_t isr = r->ISR;
        if (isr & mask) return 1;
    }
    return 0;
}

/* Wait until BUSY is clear. Returns 1 if free, 0 on timeout. */
static int i2c_wait_bus_free(I2C_TypeDef *r)
{
    volatile uint32_t n = I2C_WAIT_TIMEOUT;
    while (n--) {
        if (!(r->ISR & I2C_ISR_BUSY)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Master transfer (polling)                                         */
/* ------------------------------------------------------------------ */

int i2c_hal_master_write(i2c_hal_handle_t *h, uint16_t addr,
                          const uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;

    if (!i2c_wait_bus_free(r)) return -1;                  /* stuck bus */

    /* Program NBYTES, slave address, direction (write), START, AUTOEND */
    r->CR2 = ((uint32_t)addr << I2C_CR2_SADD_Pos)         /* SADD = addr */
           | ((uint32_t)len << I2C_CR2_NBYTES_Pos)        /* NBYTES = len */
           | I2C_CR2_START                                 /* generate START */
           | I2C_CR2_AUTOEND;                              /* auto STOP after NBYTES */

    /* Wait TXIS or NACKF */
    if (!i2c_wait_isr(r, I2C_ISR_TXIS | I2C_ISR_NACKF)) { r->CR2 |= I2C_CR2_STOP; return -1; }
    if (r->ISR & I2C_ISR_NACKF) { r->ICR |= I2C_ICR_NACKCF; r->CR2 |= I2C_CR2_STOP; return -1; }

    for (uint16_t i = 0; i < len; i++) {
        r->TXDR = buf[i];                                  /* write data byte */
        if (i < len - 1) {
            /* Wait TXIS for next byte */
            if (!i2c_wait_isr(r, I2C_ISR_TXIS | I2C_ISR_NACKF)) { r->CR2 |= I2C_CR2_STOP; return -1; }
            if (r->ISR & I2C_ISR_NACKF) { r->ICR |= I2C_ICR_NACKCF; r->CR2 |= I2C_CR2_STOP; return -1; }
        }
    }

    /* Wait for STOPF (auto-generated by AUTOEND) */
    if (!i2c_wait_isr(r, I2C_ISR_STOPF)) return -1;
    r->ICR |= I2C_ICR_STOPCF;                              /* clear STOPF */
    return 0;
}

int i2c_hal_master_start_addr(i2c_hal_handle_t *h, uint16_t addr, int is_write)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;

    if (!i2c_wait_bus_free(r)) return -1;

    /* NBYTES=1 minimum — the DMA path must set a new NBYTES before each
     * data segment. This is a KNOWN MISMATCH with the F4 API:
     * master_start_addr(h, addr, is_write) has no 'len' parameter, but the
     * F7 I2C requires NBYTES before START. For now we conservatively set
     * NBYTES=1 so the ADDR phase completes; the DMA driver must override
     * CR2 before the actual data phase. */
    r->CR2 = ((uint32_t)addr << I2C_CR2_SADD_Pos)         /* SADD = addr */
           | (1UL << I2C_CR2_NBYTES_Pos)                  /* NBYTES = 1 (placeholder) */
           | I2C_CR2_START                                 /* START */
           | (is_write ? 0UL : I2C_CR2_RD_WRN);           /* direction */

    /* Wait for ADDR or NACKF */
    if (!i2c_wait_isr(r, I2C_ISR_ADDR | I2C_ISR_NACKF)) { r->CR2 |= I2C_CR2_STOP; return -1; }
    if (r->ISR & I2C_ISR_NACKF) { r->ICR |= I2C_ICR_NACKCF; r->CR2 |= I2C_CR2_STOP; return -1; }

    /* Clear ADDR (F7 style: write ICR.ADDRCF instead of reading SR2) */
    r->ICR |= I2C_ICR_ADDRCF;

    return 0;
}

int i2c_hal_master_read(i2c_hal_handle_t *h, uint16_t addr,
                        uint8_t *buf, uint16_t len)
{
    if (!h || addr > 0x7FU) return -1;
    I2C_TypeDef *r = h->reg;

    if (!i2c_wait_bus_free(r)) return -1;

    /* Program NBYTES, slave address, direction (read), START, AUTOEND */
    r->CR2 = ((uint32_t)addr << I2C_CR2_SADD_Pos)         /* SADD = addr */
           | ((uint32_t)len << I2C_CR2_NBYTES_Pos)        /* NBYTES = len */
           | I2C_CR2_RD_WRN                                /* read direction */
           | I2C_CR2_START                                 /* generate START */
           | I2C_CR2_AUTOEND;                              /* auto STOP */

    if (len == 0) {                                         /* probe: just STOP */
        r->CR2 |= I2C_CR2_STOP;
        return 0;
    }

    for (uint16_t i = 0; i < len; i++) {
        /* Wait RXNE or NACKF */
        if (!i2c_wait_isr(r, I2C_ISR_RXNE | I2C_ISR_NACKF)) { r->CR2 |= I2C_CR2_STOP; return -1; }
        if (r->ISR & I2C_ISR_NACKF) { r->ICR |= I2C_ICR_NACKCF; r->CR2 |= I2C_CR2_STOP; return -1; }
        buf[i] = (uint8_t)r->RXDR;
    }

    /* Wait for STOPF */
    if (!i2c_wait_isr(r, I2C_ISR_STOPF)) return -1;
    r->ICR |= I2C_ICR_STOPCF;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  readback helpers (for self-test verification)                     */
/* ------------------------------------------------------------------ */

uint32_t i2c_hal_get_ccr(i2c_hal_handle_t *h)
    { return h ? h->reg->TIMINGR : 0UL; }                 /* maps to TIMINGR */

uint32_t i2c_hal_get_cr2_freq(i2c_hal_handle_t *h)
    { return h ? (h->reg->CR2 & I2C_CR2_NBYTES_Msk) : 0UL; }

uint32_t i2c_hal_get_cr1(i2c_hal_handle_t *h)
    { return h ? h->reg->CR1 : 0UL; }

int i2c_hal_is_busy(i2c_hal_handle_t *h)
    { return h ? ((h->reg->ISR & I2C_ISR_BUSY) ? 1 : 0) : 0; }

/* ------------------------------------------------------------------ */
/*  Interrupt-mode helpers (F7-style ISR/ICR, no EV/ER split)         */
/* ------------------------------------------------------------------ */

irq_id_t i2c_hal_ev_irq_id(i2c_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)I2C1_BASE) return (irq_id_t)I2C1_EV_IRQn;
    else if (p == (void *)I2C2_BASE) return (irq_id_t)I2C2_EV_IRQn;
    else if (p == (void *)I2C3_BASE) return (irq_id_t)I2C3_EV_IRQn;
    else if (p == (void *)I2C4_BASE) return (irq_id_t)I2C4_EV_IRQn;
    return -1;
}

irq_id_t i2c_hal_er_irq_id(i2c_hal_handle_t *h)
{
    if (!h) return -1;
    void *p = (void *)h->reg;
    if      (p == (void *)I2C1_BASE) return (irq_id_t)I2C1_ER_IRQn;
    else if (p == (void *)I2C2_BASE) return (irq_id_t)I2C2_ER_IRQn;
    else if (p == (void *)I2C3_BASE) return (irq_id_t)I2C3_ER_IRQn;
    else if (p == (void *)I2C4_BASE) return (irq_id_t)I2C4_ER_IRQn;
    return -1;
}

void i2c_hal_enable_ev_irq(i2c_hal_handle_t *h)
    { if (h) { h->reg->CR1 |= I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE; } }

void i2c_hal_disable_ev_irq(i2c_hal_handle_t *h)
    { if (h) { h->reg->CR1 &= ~(I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_TCIE); } }

void i2c_hal_enable_er_irq(i2c_hal_handle_t *h)
    { if (h) { h->reg->CR1 |= I2C_CR1_NACKIE | I2C_CR1_STOPIE; } }

void i2c_hal_disable_er_irq(i2c_hal_handle_t *h)
    { if (h) { h->reg->CR1 &= ~(I2C_CR1_NACKIE | I2C_CR1_STOPIE); } }

uint32_t i2c_hal_read_sr1(i2c_hal_handle_t *h)  { return h ? h->reg->ISR : 0UL; }
uint32_t i2c_hal_read_sr2(i2c_hal_handle_t *h)
    { if (h) h->reg->ICR |= I2C_ICR_ADDRCF; return 0UL; }     /* ADDR clear → SR2 stub */

void i2c_hal_write_dr(i2c_hal_handle_t *h, uint8_t data) { if (h) h->reg->TXDR = data; }
uint8_t i2c_hal_read_dr(i2c_hal_handle_t *h)      { return h ? (uint8_t)h->reg->RXDR : 0U; }
void i2c_hal_set_start(i2c_hal_handle_t *h)       { if (h) h->reg->CR2 |= I2C_CR2_START; }
void i2c_hal_set_stop(i2c_hal_handle_t *h)        { if (h) h->reg->CR2 |= I2C_CR2_STOP; }
void i2c_hal_set_ack(i2c_hal_handle_t *h, int on)
    { if (h) { if (on) h->reg->CR1 |= I2C_CR1_ADDRIE; else h->reg->CR1 &= ~I2C_CR1_ADDRIE; } }
void i2c_hal_set_pos(i2c_hal_handle_t *h, int on) { (void)h; (void)on; }  /* no POS in F7 */

void i2c_hal_clear_sr1_af(i2c_hal_handle_t *h)
    { if (h) { h->reg->ICR |= I2C_ICR_NACKCF; h->reg->CR2 |= I2C_CR2_STOP; } }

void i2c_hal_nvic_enable(int irq)                  { if (irq >= 0) NVIC_EnableIRQ((IRQn_Type)irq); }
void i2c_hal_nvic_disable(int irq)                 { if (irq >= 0) NVIC_DisableIRQ((IRQn_Type)irq); }

/* DMA helpers — CR2.DMAEN / CR2.LAST exist but live in different positions
 * on F7. We map them through the same API names. */
void i2c_hal_dma_enable(i2c_hal_handle_t *h, int on)
    { if (h) { if (on) h->reg->CR1 |= I2C_CR1_TXDMAEN | I2C_CR1_RXDMAEN; else h->reg->CR1 &= ~(I2C_CR1_TXDMAEN | I2C_CR1_RXDMAEN); } }

void i2c_hal_set_dma_last(i2c_hal_handle_t *h, int on)
    { (void)h; (void)on; }                                     /* no LAST in F7; use NBYTES */

void *i2c_hal_get_dr_addr(i2c_hal_handle_t *h)      { return h ? (void *)&h->reg->TXDR : NULL; }

int i2c_hal_wait_btf(i2c_hal_handle_t *h, uint32_t timeout)
{
    if (!h) return 0;
    volatile uint32_t n = timeout;
    while (n--) { if (h->reg->ISR & I2C_ISR_TC) return 1; }    /* BTF → TC on F7 */
    return 0;
}