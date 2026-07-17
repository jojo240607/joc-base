#include "adc_stm32.h"
#include <stdlib.h>
#include <string.h>

/* ADC prescaler: PCLK2 (84 MHz) / 4 = 21 MHz (max ADC clock is 36 MHz) */
#define ADC_PRESCALE_DIV4   (0x1UL << 16)   /* ADC_CCR_ADCPRE_0 */
/* 480 cycles sampling time (long enough for internal temp/vref sensors) */
#define ADC_SMP_480         (0x7UL)

/* channel -> (GPIO port, pin) for ADC1 external channels 0..15 */
static GPIO_TypeDef *const adc_gpio_port[16] = {
    GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA,  /* 0..7  */
    GPIOB, GPIOB,                                            /* 8..9  */
    GPIOC, GPIOC, GPIOC, GPIOC, GPIOC, GPIOC                /* 10..15 */
};
static const uint8_t adc_gpio_pin[16] = {
    0, 1, 2, 3, 4, 5, 6, 7,  0, 1,  0, 1, 2, 3, 4, 5
};

static uint32_t adc_stm32_vread_ret(adc_stm32 *self);
static uint32_t adc_stm32_vread_mv(adc_stm32 *self);
static void adc_stm32_vset_channel(adc_stm32 *self, uint32_t channel);
static void adc_stm32_hw_init(adc_stm32 *self);
static void adc_stm32_config_gpio(uint32_t channel);
static void adc_stm32_set_sampling(adc_stm32 *self);

const struct adc_stm32Fun adc_stm32_fun = {
    .destroy     = adc_stm32_destroy,
    .init        = adc_stm32_init,
    .deinit      = adc_stm32_deinit,
    .read        = adc_stm32_vread_ret,
    .read_mv     = adc_stm32_vread_mv,
    .set_channel = adc_stm32_vset_channel,
};

adc_stm32 *adc_stm32_create(ADC_TypeDef *adc, uint32_t channel)
{
    adc_stm32 *self = (adc_stm32 *)malloc(sizeof(adc_stm32));
    if (!self) return NULL;
    memset(self, 0, sizeof(adc_stm32));
    self->instance = adc;
    self->channel  = channel;
    self->vdda_mv  = 3300UL;
    self->internal = (channel >= 16) ? 1 : 0;
    adc_stm32_init(self);
    return self;
}

void adc_stm32_destroy(adc_stm32 *self)
{
    if (!self) return;
    adc_stm32_deinit(self);
    free(self);
}

void adc_stm32_init(adc_stm32 *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct adc_stm32Vtable *)malloc(sizeof(struct adc_stm32Vtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct adc_stm32Vtable));
    }
    self->fun = &adc_stm32_fun;
    self->vtable->read       = adc_stm32_vread_ret;
    self->vtable->read_mv    = adc_stm32_vread_mv;
    self->vtable->set_channel = adc_stm32_vset_channel;

    adc_stm32_hw_init(self);
}

void adc_stm32_deinit(adc_stm32 *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

uint32_t adc_stm32_read(adc_stm32 *self)
{
    if (!self || !self->vtable) return 0U;
    return self->vtable->read(self);
}

uint32_t adc_stm32_read_mv(adc_stm32 *self)
{
    if (!self || !self->vtable) return 0U;
    return self->vtable->read_mv(self);
}

void adc_stm32_set_channel(adc_stm32 *self, uint32_t channel)
{
    if (!self || !self->vtable) return;
    self->vtable->set_channel(self, channel);
}

/* --- virtual implementations --- */

static uint32_t adc_stm32_vread_ret(adc_stm32 *self)
{
    ADC_TypeDef *adc = self->instance;

    /* start a single conversion */
    adc->CR2 |= ADC_CR2_SWSTART;
    /* wait for end-of-conversion (EOCS=1 -> EOC set after each conversion) */
    while ((adc->SR & ADC_SR_EOC) == 0) { }
    return (uint32_t)(adc->DR & 0x0FFFUL);   /* 12-bit right-aligned */
}

static uint32_t adc_stm32_vread_mv(adc_stm32 *self)
{
    uint32_t raw = adc_stm32_vread_ret(self);
    return (raw * self->vdda_mv) / 4095UL;    /* 12-bit full scale */
}

static void adc_stm32_vset_channel(adc_stm32 *self, uint32_t channel)
{
    self->channel  = channel;
    self->internal = (channel >= 16) ? 1 : 0;
    adc_stm32_hw_init(self);
}

/* --- hardware setup --- */

static void adc_stm32_config_gpio(uint32_t channel)
{
    if (channel >= 16U) return;   /* internal channels need no gpio */

    GPIO_TypeDef *port = adc_gpio_port[channel];
    uint8_t pin = adc_gpio_pin[channel];

    /* enable the gpio port clock */
    if (port == GPIOA)      RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (port == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;

    /* analog mode (MODER = 0b11), no pull */
    port->MODER = (port->MODER & ~(3U << (pin * 2))) | (3U << (pin * 2));
    port->PUPDR &= ~(3U << (pin * 2));
}

static void adc_stm32_set_sampling(adc_stm32 *self)
{
    ADC_TypeDef *adc = self->instance;
    uint32_t ch = self->channel;

    if (ch <= 9U) {
        uint32_t shift = ch * 3U;
        adc->SMPR2 = (adc->SMPR2 & ~(7U << shift)) | (ADC_SMP_480 << shift);
    } else if (ch <= 18U) {
        uint32_t shift = (ch - 10U) * 3U;
        adc->SMPR1 = (adc->SMPR1 & ~(7U << shift)) | (ADC_SMP_480 << shift);
    }
}

static void adc_stm32_hw_init(adc_stm32 *self)
{
    ADC_TypeDef *adc = self->instance;

    /* 1. enable ADC clock (all three ADCs share APB2) */
    if (adc == ADC1)      RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    else if (adc == ADC2) RCC->APB2ENR |= RCC_APB2ENR_ADC2EN;
    else if (adc == ADC3) RCC->APB2ENR |= RCC_APB2ENR_ADC3EN;

    /* 2. common config: prescaler, and enable internal sensors if needed */
    ADC123_COMMON->CCR = (ADC123_COMMON->CCR & ~ADC_CCR_ADCPRE) | ADC_PRESCALE_DIV4;
    if (self->channel == 16U || self->channel == 17U)
        ADC123_COMMON->CCR |= ADC_CCR_TSVREFE;   /* temp sensor + VREFINT */
    else if (self->channel == 18U)
        ADC123_COMMON->CCR |= ADC_CCR_VBATE;     /* VBAT */

    /* 3. gpio for external channels */
    adc_stm32_config_gpio(self->channel);

    /* 4. CR1: 12-bit resolution, no scan/discontinuos */
    adc->CR1 &= ~ADC_CR1_RES;

    /* 5. CR2: EOC after each conversion, single (non-continuous), right aligned */
    adc->CR2 = ADC_CR2_EOCS;

    /* 6. sampling time for the selected channel */
    adc_stm32_set_sampling(self);

    /* 7. regular sequence: 1 conversion, channel = SQ1 */
    adc->SQR1 = 0U;                       /* L = 0 -> 1 conversion */
    adc->SQR3 = (self->channel & 0x1FUL);

    /* 8. enable the ADC and wait for stabilization */
    adc->CR2 |= ADC_CR2_ADON;
    for (volatile uint32_t i = 0; i < 1000UL; i++) { }

    /* 9. discard the first (unstable) conversion */
    adc->CR2 |= ADC_CR2_SWSTART;
    while ((adc->SR & ADC_SR_EOC) == 0) { }
    (void)adc->DR;
}
