#include "i2s_hal.h"
#include "stm32f4xx.h"     /* SPI_TypeDef, RCC, SPIx_BASE + I2S bit defs */
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — I2S (STM32F4, SPI-hosted).
 */

/* Per-wait timeout (tight-loop iterations) for TXE polling. */
#define I2S_TIMEOUT  200000U

/* HSE frequency on the Discovery board (matches clock_hal.c). */
#define I2S_HSE_HZ   8000000UL

struct i2s_hal_handle {
    SPI_TypeDef *reg;
    uint32_t     i2s_clk;   /* PLLI2S output frequency, set by config_pll */
    uint32_t     audio_hz;  /* requested sample rate */
    uint32_t     datlen;    /* 0=16,1=24,2=32 */
};

i2s_hal_handle_t *i2s_hal_create(void *peripheral)
{
    if (!peripheral) return NULL;
    i2s_hal_handle_t *h = (i2s_hal_handle_t *)malloc(sizeof(i2s_hal_handle_t));
    if (!h) return NULL;
    h->reg = (SPI_TypeDef *)peripheral;
    h->i2s_clk = 0U;
    h->audio_hz = 0U;
    h->datlen = 0U;
    return h;
}

void i2s_hal_destroy(i2s_hal_handle_t *h) { free(h); }

void i2s_hal_enable_clock(i2s_hal_handle_t *h)
{
    if (!h) return;
    void *p = (void *)h->reg;
    if      (p == (void *)SPI1_BASE) RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    else if (p == (void *)SPI2_BASE) RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
    else if (p == (void *)SPI3_BASE) RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;
}

uint32_t i2s_hal_config_pll(i2s_hal_handle_t *h, uint32_t plli2sn, uint32_t plli2sr)
{
    (void)h;
    /* PLLI2S input = HSE / PLLM (the main PLL's /M also feeds PLLI2S). */
    uint32_t m = (RCC->PLLCFGR & RCC_PLLCFGR_PLLM) >> RCC_PLLCFGR_PLLM_Pos;
    if (m == 0U) m = 1U;

    RCC->PLLI2SCFGR = ((plli2sn & 0x1FFU) << RCC_PLLI2SCFGR_PLLI2SN_Pos)
                    | ((plli2sr & 0x7U)   << RCC_PLLI2SCFGR_PLLI2SR_Pos);
    RCC->CFGR &= ~RCC_CFGR_I2SSRC;      /* I2SSRC = 0 => clock from PLLI2S */
    RCC->CR   |= RCC_CR_PLLI2SON;        /* start PLLI2S */
    while ((RCC->CR & RCC_CR_PLLI2SRDY) == 0) { }

    uint32_t i2s_clk = (I2S_HSE_HZ / m) * plli2sn / plli2sr;
    if (h) h->i2s_clk = i2s_clk;
    return i2s_clk;
}

void i2s_hal_config(i2s_hal_handle_t *h, uint32_t std, uint32_t datlen,
                    uint32_t audio_hz, uint32_t i2s_clk_hz, int master, int tx)
{
    if (!h) return;
    SPI_TypeDef *r = h->reg;

    /* I2S requires the SPI to be DISABLED (CR1.SPE = 0) before I2SMOD is set. */
    r->CR1 = 0;

    /* Compute the I2S prescaler so the resulting WS frequency == audio_hz.
     *   packetlength = 16 (16-bit data) or 32 (24/32-bit data)
     *   tmpreg = i2s_clk / (audio_hz * packetlength * 2)
     *   then split into I2SDIV (even part) + I2SODD. */
    uint32_t packetlength = (datlen == 0U) ? 16U : 32U;
    uint32_t tmpreg = 0U;
    if (audio_hz != 0U && packetlength != 0U)
        tmpreg = i2s_clk_hz / (audio_hz * packetlength * 2U);
    uint32_t odd = 0U, div = 0U;
    if (tmpreg == 0U) tmpreg = 2U;
    if (tmpreg & 1U) { odd = 1U; div = (tmpreg - 1U) / 2U; }
    else              { odd = 0U; div = tmpreg / 2U; }
    if (div < 2U) div = 2U;   /* I2SDIV must be >= 2 */

    /* I2SCFGR: I2SMOD=1, I2SE=0 (enabled later), I2SCFG=master/slave x tx/rx,
     * I2SSTD=std, CKPOL=0, PCMSYNC=0, DATLEN=datlen, CHLEN=0(16-bit)/1(24/32). */
    uint32_t cfg = SPI_I2SCFGR_I2SMOD;
    uint32_t i2scfg = 0U;
    if (master) i2scfg = tx ? 0x2U : 0x3U;     /* 10=master TX, 11=master RX */
    else        i2scfg = tx ? 0x0U : 0x1U;     /* 00=slave TX,  01=slave RX */
    cfg |= (i2scfg & 0x3U) << 8;
    cfg |= (std & 0x3U) << 4;                  /* I2SSTD */
    cfg |= (datlen & 0x3U) << 1;               /* DATLEN */
    if (datlen == 0U) cfg &= ~((uint32_t)SPI_I2SCFGR_CHLEN);  /* 16-bit channel */
    else              cfg |=  SPI_I2SCFGR_CHLEN;                  /* 32-bit channel */
    r->I2SCFGR = (uint16_t)cfg;

    /* I2SPR: MCKOE=0 (no master-clock output), ODD, I2SDIV. */
    r->I2SPR = (uint16_t)(((odd & 0x1U) << 8) | (div & 0xFFU));

    h->i2s_clk = i2s_clk_hz;
    h->audio_hz = audio_hz;
    h->datlen = datlen;
}

void i2s_hal_enable(i2s_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->I2SCFGR |= SPI_I2SCFGR_I2SE;
    else    h->reg->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
}

int i2s_hal_write_sample(i2s_hal_handle_t *h, uint16_t sample)
{
    if (!h) return -1;
    SPI_TypeDef *r = h->reg;
    volatile uint32_t tmo = I2S_TIMEOUT;
    while (!(r->SR & SPI_SR_TXE)) { if (--tmo == 0) return -1; }
    r->DR = sample;     /* 16-bit access (DR is uint16_t in the CMSIS struct) */
    return 0;
}

int i2s_hal_tx_empty(i2s_hal_handle_t *h)
{
    return h ? ((h->reg->SR & SPI_SR_TXE) ? 1 : 0) : 0;
}

uint32_t i2s_hal_get_i2scfgr(i2s_hal_handle_t *h) { return h ? h->reg->I2SCFGR : 0UL; }
uint32_t i2s_hal_get_i2spr(i2s_hal_handle_t *h)   { return h ? h->reg->I2SPR   : 0UL; }
uint32_t i2s_hal_get_plli2s(void)                 { return RCC->PLLI2SCFGR; }
uint32_t i2s_hal_pll_rdy(void)                    { return (RCC->CR & RCC_CR_PLLI2SRDY) ? 1U : 0U; }
uint32_t i2s_hal_get_cfgr(void)                   { return RCC->CFGR; }
uint32_t i2s_hal_get_i2s_clk_hz(i2s_hal_handle_t *h) { return h ? h->i2s_clk : 0UL; }
