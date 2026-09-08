#include "adc_hal.h"
#include "stm32f103xx.h"
#include <stdlib.h>

/* 480 cycles sampling time (long enough for internal temp/vref sensors) */
#define ADC_SMP_480         (0x7UL)

struct adc_hal_handle {
    ADC_TypeDef *adc;
    uint32_t channel;
};

/* channel -> (GPIO port, pin) for ADC1 external channels 0..15 */
static GPIO_TypeDef *const adc_gpio_port[16] = {
    GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA,  /* 0..7  */
    GPIOB, GPIOB,                                            /* 8..9  */
    GPIOC, GPIOC, GPIOC, GPIOC, GPIOC, GPIOC                /* 10..15 */
};
static const uint8_t adc_gpio_pin[16] = {
    0, 1, 2, 3, 4, 5, 6, 7,  0, 1,  0, 1, 2, 3, 4, 5
};

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

void adc_hal_enable_clock(adc_hal_handle_t *h)
{
    if (!h) return;
    if (h->adc == ADC1)      RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    else if (h->adc == ADC2) RCC->APB2ENR |= RCC_APB2ENR_ADC2EN;
}

void adc_hal_common_config(adc_hal_handle_t *h)
{
    if (!h) return;
    /* F103 has no ADC_CCR: ADC clock prescaler is in RCC_CFGR (ADCPRE bits 15:14).
     * Default after reset is PCLK2/2, which is adequate for APB2 <= 72 MHz
     * divided down to <= 14 MHz (max ADC clock). No change needed here. */

    /* TSVREFE on F103 lives in ADC_CR2 bit 23 (not in a common register). */
    if (h->channel == 16U || h->channel == 17U)
        h->adc->CR2 |= ADC_CR2_TSVREFE;   /* temp sensor + VREFINT */
}

void adc_hal_config_gpio(adc_hal_handle_t *h)
{
    if (!h || h->channel >= 16U) return;   /* internal channels need no gpio */

    GPIO_TypeDef *port = adc_gpio_port[h->channel];
    uint8_t pin = adc_gpio_pin[h->channel];

    /* F103 GPIO clock enable (APB2) */
    if (port == GPIOA)      RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    else if (port == GPIOB) RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;
    else if (port == GPIOC) RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    else if (port == GPIOD) RCC->APB2ENR |= RCC_APB2ENR_IOPDEN;
    else if (port == GPIOE) RCC->APB2ENR |= RCC_APB2ENR_IOPEEN;

    /* F103 GPIO analog mode: CNF=0b00 (analog), MODE=0b00 (input) */
    if (pin < 8U) {
        port->CRL = (port->CRL & ~(0xFU << (pin * 4U)));
    } else {
        port->CRH = (port->CRH & ~(0xFU << ((pin - 8U) * 4U)));
    }
}

void adc_hal_config_channel(adc_hal_handle_t *h)
{
    if (!h) return;
    ADC_TypeDef *adc = h->adc;
    uint32_t channel = h->channel;

    /* 12-bit only (no RES bits on F103), EOC after each conversion */
    adc->CR2 = 0U;

    /* sampling time: 480 cycles for the selected channel */
    if (channel <= 9U) {
        uint32_t shift = channel * 3U;
        adc->SMPR2 = (adc->SMPR2 & ~(7U << shift)) | (ADC_SMP_480 << shift);
    } else if (channel <= 18U) {
        uint32_t shift = (channel - 10U) * 3U;
        adc->SMPR1 = (adc->SMPR1 & ~(7U << shift)) | (ADC_SMP_480 << shift);
    }

    /* regular sequence: 1 conversion, channel = SQ1 */
    adc->SQR1 = 0U;                       /* L = 0 -> 1 conversion */
    adc->SQR3 = (channel & 0x1FUL);

    /* enable the ADC and wait for stabilization */
    adc->CR2 |= ADC_CR2_ADON;
    for (volatile uint32_t i = 0; i < 1000UL; i++) { }

    /* Discard the first (unstable) conversion. Mask EOCIE around the wait. */
    uint32_t eocie = adc->CR1 & ADC_CR1_EOCIE;
    adc->CR1 &= ~ADC_CR1_EOCIE;
    adc->CR2 |= ADC_CR2_SWSTART;
    while ((adc->SR & ADC_SR_EOC) == 0) { }
    (void)adc->DR;
    adc->CR1 |= eocie;
}

void adc_hal_set_channel(adc_hal_handle_t *h, uint32_t channel)
{
    if (!h) return;
    h->channel = channel;
    adc_hal_common_config(h);
    adc_hal_config_gpio(h);
    adc_hal_config_channel(h);
}

uint32_t adc_hal_single_convert(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    ADC_TypeDef *adc = h->adc;
    uint32_t eocie = adc->CR1 & ADC_CR1_EOCIE;
    adc->CR1 &= ~ADC_CR1_EOCIE;
    adc->CR2 |= ADC_CR2_SWSTART;
    while ((adc->SR & ADC_SR_EOC) == 0) { }
    uint32_t raw = (uint32_t)(adc->DR & 0x0FFFUL);
    adc->CR1 |= eocie;
    return raw;
}

irq_id_t adc_hal_irq_id(adc_hal_handle_t *h)
{
    (void)h;
    return (irq_id_t)ADC1_2_IRQn;
}

void adc_hal_enable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->CR1 |= ADC_CR1_EOCIE;
}

void adc_hal_disable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->CR1 &= ~ADC_CR1_EOCIE;
}

void adc_hal_start_convert(adc_hal_handle_t *h)
{
    if (h) h->adc->CR2 |= ADC_CR2_SWSTART;
}

uint32_t adc_hal_read_dr(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    return (uint32_t)(h->adc->DR & 0x0FFFUL);
}

void *adc_hal_get_dr_addr(adc_hal_handle_t *h)
{
    return h ? (void *)&h->adc->DR : NULL;
}

void adc_hal_enable_dma(adc_hal_handle_t *h)
{
    if (!h) return;
    h->adc->CR2 |= (ADC_CR2_DMA | ADC_CR2_CONT | ADC_CR2_SWSTART);
}

void adc_hal_disable_dma(adc_hal_handle_t *h)
{
    if (!h) return;
    h->adc->CR2 &= ~(ADC_CR2_DMA | ADC_CR2_CONT);
}

uint32_t adc_hal_to_mv(uint32_t raw, uint32_t vdda_mv)
{
    return (raw * vdda_mv) / 4095UL;
}