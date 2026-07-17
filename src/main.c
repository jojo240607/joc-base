/**
 * STM32F4 Discovery (STM32F407VGT6) minimal OOC example
 *  - Clock  : HSE(8 MHz) -> PLL -> 168 MHz            (clock class)
 *  - Serial : USART1 (PA9=TX, PA10=RX) @ 115200 8N1  (uart_stm32 class)
 *             -> external USB-TTL -> PC COM8
 *  - LED    : green LD4 on PD12                       (gpio_pin class)
 *  - BIST   : on-board self-test                       (selftest class)
 *
 * All peripherals are modelled as OOC objects following the moban/ template
 * (vtable + fun + create/destroy/init/deinit).
 *
 * After BIST the firmware enters a line-based command loop so a PC companion
 * test (tools/companion_test.py) can verify the TX/RX loopback:
 *   PING         -> PONG
 *   ECHO <text>  -> <text>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "clock.h"
#include "serial.h"
#include "uart_stm32.h"
#include "gpio_pin.h"
#include "adc_stm32.h"
#include "selftest.h"

int main(void)
{
    /* 1. system clock -> 168 MHz (HSE -> PLL) */
    clock *clk = clock_create();

    /* 2. USART1 console on PA9/PA10 @ 115200 */
    uart_stm32 *uart = uart_stm32_create(USART1, 115200UL);
    uart_stm32_set_console(uart);

    /* 3. green LED (LD4) on PD12 as an output pin object */
    gpio_pin *led = gpio_pin_create(GPIOD, 12, 1);   /* mode 1 = output */

    /* 3b. ADC1 on PA0 (channel 0); PA0 is the on-board blue button (pulled high) */
    adc_stm32 *adc = adc_stm32_create(ADC1, 0);

    printf("Hello from STM32F407 Discovery (OOC)!\r\n");
    printf("System clock: %lu Hz, USART1 @ 115200 8N1\r\n",
           (unsigned long)clock_get_sysclk_hz(clk));

    /* 4. on-board self-test (BIST) at boot */
    selftest *st = selftest_create(clk, uart, led, adc);
    selftest_run(st);

    printf("READY. Commands: PING / ECHO <text> / BIST / ADC\r\n");

    /* 5. command loop (PC companion test exercises this) */
    char line[64];
    uint32_t idx = 0;
    while (1)
    {
        char c = uart_stm32_getc(uart);
        uart_stm32_console_putc(c);          /* local echo for terminal use */

        if (c == '\r' || c == '\n')
        {
            if (idx > 0)
            {
                line[idx] = '\0';
                idx = 0;
                gpio_pin_toggle(led);        /* visible activity per command */
                if (strcmp(line, "PING") == 0)
                    printf("PONG\r\n");
                else if (strncmp(line, "ECHO ", 5) == 0)
                    printf("%s\r\n", line + 5);
                else if (strcmp(line, "BIST") == 0)
                    selftest_run(st);        /* re-run self-test on demand */
                else if (strncmp(line, "ADC", 3) == 0) {
                    uint32_t ch = 0;
                    if (line[3] == ' ')
                        ch = (uint32_t)atoi(line + 4);
                    if (ch > 18U) ch = 0U;
                    adc_stm32_set_channel(adc, ch);
                    uint32_t raw = adc_stm32_read(adc);
                    uint32_t mv  = adc_stm32_read_mv(adc);
                    adc_stm32_set_channel(adc, 0U);   /* restore PA0 */
                    printf("ADC CH%lu raw=%lu mV=%lu\r\n",
                           (unsigned long)ch, (unsigned long)raw, (unsigned long)mv);
                }
                else
                    printf("ERR unknown\r\n");
            }
        }
        else if (idx < (sizeof(line) - 1))
        {
            line[idx++] = c;
        }
    }
}
