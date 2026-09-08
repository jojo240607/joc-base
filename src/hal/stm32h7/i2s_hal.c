#include "i2s_hal.h"
#include "stm32h750xx.h"   /* SPI_TypeDef, RCC, SPIx_BASE + bit defs */
#include <stdlib.h>

/*
 * Hardware Abstraction Layer — I2S (STM32H7, SPI-hosted).
 *
 * On the STM32H7 the I2S function is hosted inside the SPI peripheral: the
 * same register block (SPI_TypeDef) serves SPI when I2SCFGR.I2SMOD=0 and I2S
 * when I2SMOD=1. This HAL operates on a SPI_TypeDef* and switches it to I2S
 * mode.
 *
 * H7 I2SCFGR layout (differs from F4):
 *   I2SMOD  @0      I2SCFG  @1-3 (3-bit!)  I2SSTD  @4-5
 *   PCMSYNC @7      DATLEN  @8-9            CHLEN   @10
 *   CKPOL   @11     FIXCH   @12             WSINV   @13
 *   DATFMT  @14     I2SDIV  @16-23 (8-bit)  ODD     @24
 *   MCKOE   @25
 *
 * The I2S bit clock is generated from a PLL (PLL2 or PLL3) and divided by
 * I2SDIV[7:0] + ODD. On H7, the audio PLL output is selected via
 * RCC_D2CCIP2R.I2SxSEL (0=PLL2, 1=PLL3, 3=Per_ck). This HAL uses a computed
 * clock strategy: the caller provides the actual I2SxCLK frequency (from
 * board-level PLL2/PLL3 calculations), and the HAL computes the prescaler.
 */

/* Per-wait timeout (tight-loop iterations). */
#define I2S_TIMEOUT  200000U

/* HSE frequency on the H750B-DK board (matches clock_hal.c). */
#define I2S_HSE_HZ   25000000UL

struct i2s_hal_handle {
    SPI_TypeDef *reg;
    uint32_t     i2s_clk;   /* PLL2/PLL3 output frequency, set by config_pll */
    uint32_t     audio_hz;  /* requested sample rate */
    uint32_t     datlen;    /* 0=16, 1=24, 2=32 */
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
    else if (p == (void *)SPI2_BASE) RCC->APB1LENR |= RCC_APB1LENR_SPI2EN;
    else if (p == (void *)SPI3_BASE) RCC->APB1LENR |= RCC_APB1LENR_SPI3EN;
}

/* On H7 the I2S clock source is PLL2 or PLL3 (not F4's dedicated PLLI2S).
 * This placeholder takes a pre-computed PLL output frequency and stores it
 * in the handle. The caller (board layer or driver) is responsible for
 * configuring PLL2/PLL3 and calling this with the resulting i2s_clk_hz.
 * The return value is the same i2s_clk_hz (passed through). */
uint32_t i2s_hal_config_pll(i2s_hal_handle_t *h, uint32_t plli2sn, uint32_t plli2sr)
{
    (void)plli2sn;
    (void)plli2sr;

    /* Placeholder: H7 uses PLL2/PLL3 instead of F4's dedicated PLLI2S.
     * The RCC_PLLCFGR register on H7 only has PLLxEN bits (no PLLM/PLLN/PLLR).
     * The real PLL2 config lives in RCC_PLL2CFGR (plln, pllp, pllq, pllr)
     * with input divider RCC_PLL2DIVR, and I2S clock source is selected via
     * RCC_D2CCIP2R.I2SxSEL. This stub returns a placeholder value.
     * A real port must configure PLL2/PLL3 and pass i2s_clk_hz directly
     * to i2s_hal_config().
     */
    uint32_t i2s_clk = 50000000UL;                    /* placeholder 50 MHz */
    if (h) h->i2s_clk = i2s_clk;
    return i2s_clk;
}

void i2s_hal_config(i2s_hal_handle_t *h, uint32_t std, uint32_t datlen,
                    uint32_t audio_hz, uint32_t i2s_clk_hz, int master, int tx)
{
    if (!h) return;
    SPI_TypeDef *r = h->reg;

    /* Disable SPI before switching to I2S mode */
    r->CR1 = 0;

    /* Compute I2S prescaler (same formula as F4):
     *   packetlength = 16 (16-bit data) or 32 (24/32-bit data)
     *   tmpreg = i2s_clk / (audio_hz * packetlength * 2)
     *   I2SDIV = even part, ODD = 1 if remainder >= 1. */
    uint32_t packetlength = (datlen == 0U) ? 16U : 32U;
    uint32_t tmpreg = 0U;
    if (audio_hz != 0U && packetlength != 0U)
        tmpreg = i2s_clk_hz / (audio_hz * packetlength * 2U);
    uint32_t odd = 0U, div = 0U;
    if (tmpreg == 0U) tmpreg = 2U;
    if (tmpreg & 1U) { odd = 1U; div = (tmpreg - 1U) / 2U; }
    else              { odd = 0U; div = tmpreg / 2U; }
    if (div < 2U) div = 2U;                        /* I2SDIV must be >= 2 */

    /* Build I2SCFGR (H7 layout):
     *   I2SMOD=1, I2SE=0 (enabled later)
     *   I2SCFG = master/slave × tx/rx (3-bit field @1-3)
     *   I2SSTD = std, CKPOL=0, PCMSYNC=0
     *   DATLEN = datlen, CHLEN = 0(16)/1(24/32)
     *   FIXCH=0, WSINV=0, DATFMT=0
     *   I2SDIV = div[7:0] @16-23, ODD = odd @24, MCKOE=1 */
    uint32_t cfg = (1U << 0U);                     /* I2SMOD = 1 */
    uint32_t i2scfg = 0U;
    if (master) i2scfg = tx ? 2U : 3U;             /* 010=master TX, 011=master RX */
    else        i2scfg = tx ? 0U : 1U;             /* 000=slave TX, 001=slave RX */
    cfg |= (i2scfg & 0x7U) << 1;                   /* I2SCFG (3-bit, H7!) */
    cfg |= (std & 0x3U) << 4;                      /* I2SSTD */
    cfg |= (datlen & 0x3U) << 8;                   /* DATLEN */
    if (datlen != 0U) cfg |= (1U << 10U);          /* CHLEN = 1 for 24/32-bit */
    /* I2SDIV + ODD + MCKOE */
    cfg |= (div & 0xFFU) << 16;                    /* I2SDIV */
    cfg |= (odd & 0x1U) << 24;                     /* ODD */
    cfg |= (1U << 25U);                             /* MCKOE (master clock output) */

    r->I2SCFGR = cfg;

    h->i2s_clk = i2s_clk_hz;
    h->audio_hz = audio_hz;
    h->datlen = datlen;
}

void i2s_hal_enable(i2s_hal_handle_t *h, int on)
{
    if (!h) return;
    if (on) h->reg->I2SCFGR |= (1U << 0U);         /* I2SMOD is bit 0; I2SE is bit 0
                                                     * on F4 but on H7 I2SMOD=I2SE=0 works
                                                     * differently. Actually on H7:
                                                     * I2SE is NOT a separate bit —
                                                     * I2SMOD=1 + I2SCFG!=0 enables I2S.
                                                     * Re-enable with SPE=1 after config. */
    else    h->reg->CR1 &= ~SPI_CR1_SPE;
    /* I2S is enabled by SPE=1 with I2SMOD=1 */
    if (on) h->reg->CR1 |= SPI_CR1_SPE;
    else    h->reg->CR1 &= ~SPI_CR1_SPE;
}

int i2s_hal_write_sample(i2s_hal_handle_t *h, uint16_t sample)
{
    if (!h) return -1;
    SPI_TypeDef *r = h->reg;
    volatile uint32_t tmo = I2S_TIMEOUT;
    while (!(r->SR & SPI_SR_TXP)) { if (--tmo == 0) return -1; }    /* wait TXP */
    r->TXDR = sample;
    return 0;
}

int i2s_hal_tx_empty(i2s_hal_handle_t *h)
{
    return h ? ((h->reg->SR & SPI_SR_TXP) ? 1 : 0) : 0;
}

/* DMA gating: H7 v3 uses CFG1.TXDMAEN (not CR2.TXDMAEN). */
void i2s_hal_enable_tx_dma(i2s_hal_handle_t *h)  { if (h) h->reg->CFG1 |= SPI_CFG1_TXDMAEN; }
void i2s_hal_disable_tx_dma(i2s_hal_handle_t *h) { if (h) h->reg->CFG1 &= ~SPI_CFG1_TXDMAEN; }
void *i2s_hal_get_dr_addr(i2s_hal_handle_t *h)   { return h ? (void *)&h->reg->TXDR : NULL; }

/* readback helpers */
uint32_t i2s_hal_get_i2scfgr(i2s_hal_handle_t *h) { return h ? h->reg->I2SCFGR : 0UL; }
uint32_t i2s_hal_get_i2spr(i2s_hal_handle_t *h)   { return h ? (h->reg->I2SCFGR >> 16) : 0UL; }  /* I2SDIV from upper I2SCFGR */
uint32_t i2s_hal_get_plli2s(void)                 { return 0UL; }     /* no PLLI2S on H7 */
uint32_t i2s_hal_pll_rdy(void)                    { return 1U; }     /* stub */
uint32_t i2s_hal_get_cfgr(void)                   { return RCC->CFGR; }
uint32_t i2s_hal_get_i2s_clk_hz(i2s_hal_handle_t *h) { return h ? h->i2s_clk : 0UL; }