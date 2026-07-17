#include "selftest.h"
#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define UART_PCLK2_HZ 84000000UL
#define UART_BAUD     115200UL

static int selftest_vclock(selftest *self);
static int selftest_vuart(selftest *self);
static int selftest_vgpio(selftest *self);
static int selftest_vadc(selftest *self);

const struct selftestFun selftest_fun = {
    .destroy = selftest_destroy,
    .init = selftest_init,
    .deinit = selftest_deinit,
    .run = selftest_run,
};

selftest *selftest_create(clock *clk, uart_stm32 *uart, gpio_pin *led, adc_stm32 *adc)
{
    selftest *self = (selftest *)malloc(sizeof(selftest));
    if (!self) return NULL;
    memset(self, 0, sizeof(selftest));
    self->clk = clk;
    self->uart = uart;
    self->led = led;
    self->adc = adc;
    selftest_init(self);
    return self;
}

void selftest_destroy(selftest *self)
{
    if (!self) return;
    selftest_deinit(self);
    free(self);
}

void selftest_init(selftest *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct selftestVtable *)malloc(sizeof(struct selftestVtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct selftestVtable));
    }
    self->fun = &selftest_fun;
    self->vtable->test_clock = selftest_vclock;
    self->vtable->test_uart  = selftest_vuart;
    self->vtable->test_gpio  = selftest_vgpio;
    self->vtable->test_adc   = selftest_vadc;
}

void selftest_deinit(selftest *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

int selftest_run(selftest *self)
{
    if (!self || !self->vtable) return 0;

    int pass = 1;
    int r;

    printf("\r\n--- On-board self-test (BIST) ---\r\n");

    r = self->vtable->test_clock(self);
    printf("[BIST] clock : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_uart(self);
    printf("[BIST] uart  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_gpio(self);
    printf("[BIST] gpio  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_adc(self);
    printf("[BIST] adc   : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    printf("SELF-TEST: %s\r\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* --- sub-tests (virtual) --- */

static int selftest_vclock(selftest *self)
{
    int pll_locked = (RCC->CR & RCC_CR_PLLRDY) != 0;
    int pll_selected = ((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL);
    int hz_ok = (clock_get_sysclk_hz(self->clk) == 168000000UL);
    return pll_locked && pll_selected && hz_ok;
}

static int selftest_vuart(selftest *self)
{
    /* Verify the USART1 peripheral is enabled (TX+RX) and BRR is correct.
       We intentionally do NOT transmit here so the serial wire stays clean
       for the PC companion test (the RX/TX loopback is checked there). */
    USART_TypeDef *usart = self->uart->instance;
    int enabled = (usart->CR1 & (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE))
                  == (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
    int br_ok = (usart->BRR == (uint32_t)(UART_PCLK2_HZ / UART_BAUD));
    return enabled && br_ok;
}

static int selftest_vgpio(selftest *self)
{
    uint8_t before = gpio_pin_read(self->led);
    gpio_pin_toggle(self->led);
    uint8_t after = gpio_pin_read(self->led);
    if (before != after)
        gpio_pin_toggle(self->led);   /* restore original level */
    return before != after;
}

static int selftest_vadc(selftest *self)
{
    adc_stm32 *adc = self->adc;
    if (!adc) return 0;

    /* Read the internal VREFINT (channel 17, ~1.21 V). This exercises the
       ADC clock, sequence programming, EOC polling and data read path with
       no external wiring. At VDDA~3.3V a 12-bit reading lands near 1500;
       accept a wide sane band in case VDDA differs. */
    uint32_t saved = adc->channel;
    adc_stm32_set_channel(adc, 17U);          /* VREFINT */
    uint32_t vref = adc_stm32_read(adc);
    adc_stm32_set_channel(adc, saved);        /* restore external channel */

    int vref_ok = (vref >= 800U && vref <= 2200U);
    printf("       VREFINT raw=%lu (expect ~1500) %s\r\n",
           (unsigned long)vref, vref_ok ? "" : "[OUT OF RANGE]");
    return vref_ok;
}
