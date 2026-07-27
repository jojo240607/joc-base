#include "adc_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

/* ADC prescaler: PCLK2 (84 MHz) / 4 = 21 MHz (max ADC clock is 36 MHz) */
#define ADC_PRESCALE_DIV4   (0x1UL << 16)   /* ADC_CCR_ADCPRE_0 */
/* 480 cycles sampling time (long enough for internal temp/vref sensors) */
#define ADC_SMP_480         (0x7UL)

/* OPAQUE handle — the only ADC state the HAL keeps. Hidden from the driver. */
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
    else if (h->adc == ADC3) RCC->APB2ENR |= RCC_APB2ENR_ADC3EN;
}

void adc_hal_common_config(adc_hal_handle_t *h)
{
    if (!h) return;
    ADC123_COMMON->CCR = (ADC123_COMMON->CCR & ~ADC_CCR_ADCPRE) | ADC_PRESCALE_DIV4;
    if (h->channel == 16U || h->channel == 17U)
        ADC123_COMMON->CCR |= ADC_CCR_TSVREFE;   /* temp sensor + VREFINT */
    else if (h->channel == 18U)
        ADC123_COMMON->CCR |= ADC_CCR_VBATE;     /* VBAT */
}

void adc_hal_config_gpio(adc_hal_handle_t *h)
{
    if (!h || h->channel >= 16U) return;   /* internal channels need no gpio */

    GPIO_TypeDef *port = adc_gpio_port[h->channel];
    uint8_t pin = adc_gpio_pin[h->channel];

    if (port == GPIOA)      RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (port == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;

    /* analog mode (MODER = 0b11), no pull */
    port->MODER = (port->MODER & ~(3U << (pin * 2))) | (3U << (pin * 2));
    port->PUPDR &= ~(3U << (pin * 2));
}

void adc_hal_config_channel(adc_hal_handle_t *h)
{
    if (!h) return;
    ADC_TypeDef *adc = h->adc;
    uint32_t channel = h->channel;

    /* 12-bit resolution, EOC after each conversion, right aligned */
    adc->CR1 &= ~ADC_CR1_RES;
    adc->CR2 = ADC_CR2_EOCS;

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

    /* Discard the first (unstable) conversion. The EOC flag is only used here to
     * learn the conversion finished; if the EOC interrupt (EOCIE) is enabled
     * (e.g. the driver runs in IRQ mode), the EOC ISR would read DR and clear
     * EOC the instant it is set, so this busy-wait would spin forever. Mask
     * EOCIE around the wait so the flag stays visible, then restore it. */
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
    adc_hal_common_config(h);   /* internal-sensor enable may change with channel */
    adc_hal_config_gpio(h);
    adc_hal_config_channel(h);
}

uint32_t adc_hal_single_convert(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    ADC_TypeDef *adc = h->adc;
    /* Mask EOCIE while busy-waiting on EOC: otherwise the EOC ISR clears the
     * flag the moment it is set and this loop spins forever (see config_channel). */
    uint32_t eocie = adc->CR1 & ADC_CR1_EOCIE;
    adc->CR1 &= ~ADC_CR1_EOCIE;
    adc->CR2 |= ADC_CR2_SWSTART;
    while ((adc->SR & ADC_SR_EOC) == 0) { }
    uint32_t raw = (uint32_t)(adc->DR & 0x0FFFUL);   /* 12-bit right-aligned */
    adc->CR1 |= eocie;
    return raw;
}

/* Return the chip interrupt id for this ADC so the driver can register its EOC
 * ISR through the platform-independent irq framework without naming a
 * Cortex-M / STM32 interrupt directly. On STM32F4 all ADCs share ADC_IRQn. */
irq_id_t adc_hal_irq_id(adc_hal_handle_t *h)
{
    (void)h;
    return (irq_id_t)ADC_IRQn;
}
void adc_hal_enable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->CR1 |= ADC_CR1_EOCIE;
}
void adc_hal_disable_eoc_irq(adc_hal_handle_t *h)
{
    if (h) h->adc->CR1 &= ~ADC_CR1_EOCIE;
}
/* Trigger a single regular conversion WITHOUT busy-waiting. The EOC ISR reads
 * DR (clearing EOC) when the conversion completes. */
void adc_hal_start_convert(adc_hal_handle_t *h)
{
    if (h) h->adc->CR2 |= ADC_CR2_SWSTART;
}
/* Read the data register (12-bit). Reading DR clears EOC, so the EOC ISR MUST
 * call this to stop the interrupt from re-firing. */
uint32_t adc_hal_read_dr(adc_hal_handle_t *h)
{
    if (!h) return 0U;
    return (uint32_t)(h->adc->DR & 0x0FFFUL);
}

/* Return the ADC data-register address for the DMA PAR. ADC_DR is a 32-bit
 * register whose lower 16 bits hold the right-aligned 12-bit result, so a
 * 16-bit DMA transfer reads the sample correctly. */
void *adc_hal_get_dr_addr(adc_hal_handle_t *h)
{
    return h ? (void *)&h->adc->DR : NULL;
}

/* Enable the ADC's DMA request and start a CONTINUOUS conversion burst: with
 * CR2_CONT set, each EOC raises a DMA request that moves DR -> memory; after
 * DMA has moved `count` samples it signals Transfer-Complete and disables
 * itself. CR2_DMA must be set AFTER config_channel() (which assigns CR2 = EOCS,
 * clearing DMA/CONT) and the stream must already be configured. SWSTART kicks
 * the first conversion. */
void adc_hal_enable_dma(adc_hal_handle_t *h)
{
    if (!h) return;
    h->adc->CR2 |= (ADC_CR2_DMA | ADC_CR2_CONT | ADC_CR2_SWSTART);
}

/* Stop the continuous burst and silence the DMA request. ADON is left on so a
 * later single/poll conversion still works. */
void adc_hal_disable_dma(adc_hal_handle_t *h)
{
    if (!h) return;
    h->adc->CR2 &= ~(ADC_CR2_DMA | ADC_CR2_CONT);
}

uint32_t adc_hal_to_mv(uint32_t raw, uint32_t vdda_mv)
{
    return (raw * vdda_mv) / 4095UL;    /* 12-bit full scale */
}
