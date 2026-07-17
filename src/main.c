/**
 * STM32F4 Discovery (STM32F407VGT6) minimal OOC example
 *  - Clock  : HSE(8 MHz) -> PLL -> 168 MHz            (clock class)
 *  - Serial : USART1 (PA9=TX, PA10=RX) @ 115200 8N1  (uart_stm32 class)
 *             -> external USB-TTL -> PC COM8
 *  - LED    : green LD4 on PD12                       (gpio_pin class)
 *
 * All peripherals are modelled as OOC objects following the moban/ template
 * (vtable + fun + create/destroy/init/deinit).
 */
#include <stdio.h>
#include "clock.h"
#include "serial.h"
#include "uart_stm32.h"
#include "gpio_pin.h"

int main(void)
{
    /* 1. system clock -> 168 MHz (HSE -> PLL) */
    clock *clk = clock_create();

    /* 2. USART1 console on PA9/PA10 @ 115200 */
    uart_stm32 *uart = uart_stm32_create(USART1, 115200UL);
    uart_stm32_set_console(uart);

    /* 3. green LED (LD4) on PD12 as an output pin object */
    gpio_pin *led = gpio_pin_create(GPIOD, 12, 1);   /* mode 1 = output */

    printf("Hello from STM32F407 Discovery (OOC)!\r\n");
    printf("System clock: %lu Hz, USART1 @ 115200 8N1\r\n",
           (unsigned long)clock_get_sysclk_hz(clk));

    uint32_t tick = 0;
    while (1)
    {
        gpio_pin_toggle(led);
        printf("tick %lu\r\n", (unsigned long)tick++);

        /* rough busy-loop delay (~500 ms) */
        for (volatile uint32_t i = 0;
             i < (clock_get_sysclk_hz(clk) / 4000U) * 500U; i++) { }
    }
}
