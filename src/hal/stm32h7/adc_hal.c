#include "adc_hal.h"
#include "stm32h750xx.h"
#include <stdlib.h>

/*
 * STM32H750 ADC HAL — V5 silicon layout (DMNGT data management, PCSEL,
 * 16-bit resolution with 3-bit RES field).  Clock gate: AHB1ENR.ADC12EN
 * for ADC1/2; AHB4ENR.ADC3EN for ADC3.
 *
 * ADC core clock (adc_ker_ck) must be configured by the board layer via
 * RCC_D3CCIPR.ADCSEL (default pll2_p).  The HAL uses asynchronous mode
 * (CKMODE=00) with a /16 prescaler.
 */

/* Sampling time: 810.5 cycles (0b111) — longest available on H7, closest
 * to the F4 HAL's 480 cycles. */
#define ADC_SMP_810     (0x7UL)

/* Prescaler: adc_ker_ck / 16 (async mode).  With pll2_p ~72 MHz the
 * ADC clock is ~4.5 MHz, well within the <=36 MHz spec. */
#define ADC_PRESCALE_DIV16   (0x7UL << ADC_CCR_PRESC_Pos)

/* Internal-sensor bits in CCR (same as F4). */
#define ADC_CCR_VREFEN_Pos   22U
#define ADC_CCR_TSEN_Pos     23U
#define ADC_CCR_VBATEN_Pos   24U

/* ---- opaque handle ---- */

struct adc_hal_handle {
    ADC_TypeDef *adc;
    uint32_t channel;
};

/* channel -> (GPIO port, pin) for ADC1 external channels 0..15.
 * H7 pin mapping matches F4 (PA0-7 for ch0-7, PB0-1 for ch8-9,
 * PC0-5 for ch10-15). */
static GPIO_TypeDef *const adc_gpio_port[16] = {
    GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA,
    GPIOB, GPIOB,
    GPIOC, GPIOC, GPIOC, GPIOC, GPIOC, GPIOC
};
static const uint8_t adc_gpio_pin[16] = {
    0, 1, 2, 3, 4, 5, 6, 7,  0, 1,  0, 1, 2, 3, 4, 5
};

/* ---- construction / destruction ---- */

adc_hal_handle_t *adc_hal_create(void *peripheral, uint32_t channel)
{
    adc_hal_handle_t *h = (adc_hal_handle_t *)malloc(sizeof(adc_hal_handle_t));
    if (!h) return NULL;
    h->adc = (ADC_TypeDef *)peripheral;
    h->channel = channel;
    return h;
}

void adc_hal_destroy(adc_hal_handle_t *h)
{
    free(h);
}

/* ---- clock gate ---- */

void adc_hal_enable_clock(adc_hal_handle_t *h)
{
    if (!h) return;
    if (h->adc == ADC1 || h->adc == ADC2)
        RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
    else if (h->adc == ADC3)
        RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
}

/* ---- common control register (CCR) ---- */

void adc_hal_common_config(adc_hal_handle_t *h)
{
    if (!h) return;
    ADC_Common_TypeDef *com;

    if (h->adc == ADC1 || h->adc == ADC2)
        com = ADC12_COMMON;
    else if (h->adc == ADC3)
        com = ADC3_COMMON;
    else
        return;

    /* Async clock (CKMODE=00), prescaler = /16.
     * Enable the internal sensors the caller might need (TSEN/VREFEN/VBATEN
     * are harmless to leave on but increase power draw; the driver layer
     * should gate them when unneeded).  Start with only VREFEN + TSEN
     * (VBAT is rarely needed in normal ADC use). */
    com->CCR = (com->CCR & ~(ADC_CCR_CKMODE_Msk | ADC_CCR_PRESC_Msk))
             | ADC_PRESCALE_DIV16;

    /* Enable temperature sensor + VREFINT for on-demand use by the
     * driver layer (internal channels 16/17). */
    com->CCR |= (ADC_CCR_TSEN | ADC_CCR_VREFEN);

    if (h->channel == 18U)
        com->CCR |= ADC_CCR_VBATEN;
}

/* ---- GPIO analog mode (external channels 0..15) ---- */

void adc_hal_config_gpio(adc_hal_handle_t *h)
{
    if (!h || h->channel >= 16U) return;

    GPIO_TypeDef *port = adc_gpio_port[h->channel];
    uint8_t pin = adc_gpio_pin[h->channel];

    /* H7: GPIO clocks are on AHB4, not AHB1 as on F4. */
    if (port == GPIOA)      RCC->AHB4ENR |= RCC_AHB4ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB4ENR |= RCC_AHB4ENR_GPIOCEN;
    else if (port == GPIOD) RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN;
    else if (port == GPIOE) RCC->AHB4ENR |= RCC_AHB4ENR_GPIOEEN;

    /* Analog mode (MODER = 0b11), no pull. */
    port->MODER = (port->MODER & ~(3U << (pin * 2))) | (3U << (pin * 2));
    port->PUPDR &= ~(3U << (pin * 2));
}

/* ---- per-channel configuration + ADC bringup ---- */

void adc_hal_config_channel(adc_hal_handle_t *h)
{
    if (!h) return;

    ADC_TypeDef *adc = h->adc;
    uint32_t ch = h->channel;

    /* 1. Enable the internal voltage regulator (mandatory on H7). */
    adc->CR |= ADC_CR_ADVREGEN;
    for (volatile uint32_t i = 0; i < 2000UL; i++) { }  /* ~20 us @ 480 MHz */

    /* 2. Calibrate the ADC (mandatory on H7).  Single-ended mode. */
    adc->CR |= ADC_CR_ADCAL;
    while (adc->CR & ADC_CR_ADCAL) { }

    /* 3. Configure resolution (16-bit), no external trigger, single
     *    conversion, data management = DR only (no DMA yet). */
    adc->CFGR = 0;
    /* RES=000 → 16-bit (default after reset, but reinforce it). */

    /* 4. Program pre-channel selection (PCSEL) per V5 silicon
     *    requirement — must be done before ADEN. */
    adc->PCSEL = (1U << ch);

    /* 5. Set sampling time: SMPR1 for channels 0-9, SMPR2 for 10-18. */
    if (ch <= 9U) {
        uint32_t shift = ch * 3U;
        adc->SMPR1 = (adc->SMPR1 & ~(0x7UL << shift)) | (ADC_SMP_810 << shift);
    } else if (ch <= 18U) {
        uint32_t shift = (ch - 10U) * 3U;
        adc->SMPR2 = (adc->SMPR2 & ~(0x7UL << shift)) | (ADC_SMP_810 << shift);
    }

    /* 6. Regular sequence: 1 conversion, channel = SQ1. */
    adc->SQR1 = ((ch & 0x1FUL) << ADC_SQR1_SQ1_Pos);

    /* 7. Enable the ADC and wait for ADRDY. */
    adc->ISR = ~0U;                         /* clear any stale flags */
    adc->CR |= ADC_CR_ADEN;
    while ((adc->ISR & ADC_ISR_ADRDY) == 0) { }

    /* 8. Discard the first (unstable) conversion.  Mask EOCIE so the
     *    busy-wait doesn't race with an interrupt (same pattern as F4). */
    uint32_t eocie = adc->IER & ADC_IER_EOCIE;
    adc->IER &= ~ADC_IER_EOCIE;
    adc->CR |= ADC_CR_ADSTART;
    while ((adc->ISR & ADC_ISR_EOC) == 0) { }
    (void)adc->DR;                          /* read DR → clear EOC */
    adc->IER |= eocie;
}

/* ---- channel change (full reconfig) ---- */

void adc_hal_set_channel(adc_hal_handle_t *h, uint32_t channel)
{
    if (!h) return;
    h->channel = channel;
    adc_hal_common_config(h);   /* internal-sensor enable may change */
    adc_hal_config_gpio(h);
    adc_hal_config_channel(h);
}

/* ---- single conversion (polling) ---- */

uint32_t adc_hal_single_convert(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    ADC_TypeDef *adc = h->adc;

    uint32_t eocie = adc->IER & ADC_IER_EOCIE;
    adc->IER &= ~ADC_IER_EOCIE;

    adc->CR |= ADC_CR_ADSTART;
    while ((adc->ISR & ADC_ISR_EOC) == 0) { }

    /* H7 is 16-bit; DR[15:0] holds the result. */
    uint32_t raw = (uint32_t)(adc->DR & 0xFFFFUL);
    adc->IER |= eocie;
    return raw;
}

/* ---- IRQ support ---- */

irq_id_t adc_hal_irq_id(adc_hal_handle_t *h)
{
    (void)h;
    return (irq_id_t)ADC_IRQn;   /* ADC1/2/3 share ADC_IRQn (18) on H750 */
}

void adc_hal_enable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->IER |= ADC_IER_EOCIE;
}

void adc_hal_disable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->IER &= ~ADC_IER_EOCIE;
}

void adc_hal_start_convert(adc_hal_handle_t *h)
{
    if (h) h->adc->CR |= ADC_CR_ADSTART;
}

uint32_t adc_hal_read_dr(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    return (uint32_t)(h->adc->DR & 0xFFFFUL);
}

/* ---- DMA support ---- */

void *adc_hal_get_dr_addr(adc_hal_handle_t *h)
{
    return h ? (void *)&h->adc->DR : NULL;
}

/* Enable DMA on the ADC: set DMNGT=01 (DR + DMA1 single mode) + CONT
 * (continuous conversion), then start.  The DMA stream must already be
 * configured and armed before calling this. */
void adc_hal_enable_dma(adc_hal_handle_t *h)
{
    if (!h) return;

    /* DMNGT=01 → each EOC generates a DMA1 request (non-circular).
     * CONT=1   → conversions run continuously. */
    h->adc->CFGR |= ADC_CFGR_DMNGT_0 | ADC_CFGR_CONT;
    h->adc->CR  |= ADC_CR_ADSTART;
}

void adc_hal_disable_dma(adc_hal_handle_t *h)
{
    if (!h) return;

    /* Clear DMNGT (back to DR-only) and CONT (stop continuous mode). */
    h->adc->CFGR &= ~(ADC_CFGR_DMNGT_0 | ADC_CFGR_CONT);
}

/* ---- millivolt conversion (16-bit full scale) ---- */

uint32_t adc_hal_to_mv(uint32_t raw, uint32_t vdda_mv)
{
    return (raw * vdda_mv) / 65535UL;   /* 16-bit full scale */
}