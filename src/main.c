/**
 * STM32F4 Discovery (STM32F407VGT6) minimal OOC example
 *  - Clock  : HSE(8 MHz) -> PLL -> 168 MHz            (clock class)
 *  - Serial : USART1 (PA9=TX, PA10=RX) @ 115200 8N1  (uart class)
 *             -> external USB-TTL -> PC COM8
 *  - LED    : green LD4 on PD12                       (gpio_pin class)
 *  - BIST   : on-board self-test                       (selftest class)
 *
 * All peripherals are modelled as OOC objects that implement the SAME unified
 * `device` interface (iface/device.h). The upper layer therefore holds a
 * `device *` for EVERY driver and operates on them through one virtual dispatch
 * convention — exactly the C equivalent of Java's `dev.open()` on a polymorphic
 * reference:
 *
 *      dev->vtable->open(dev);
 *      dev->vtable->read(dev, buf, len);
 *      dev->vtable->write(dev, buf, len);
 *      dev->vtable->ioctl(dev, cmd, arg);
 *      dev->vtable->close(dev);
 *
 * Typed convenience methods (when you hold the concrete pointer) go through the
 * `fun` table and are NEVER called as bare functions:
 *
 *      uart->fun->getc(uart);
 *      adc->fun->set_channel(adc, ch);
 *
 * DRIVER / HAL SPLIT (the point of this refactor):
 *   - The driver layer (drv/) is 100% platform-independent. It only ever holds
 *     an OPAQUE HAL handle (adc_hal_handle_t *, gpio_hal_handle_t *, ...) and
 *     never sees ADC_TypeDef / GPIO_TypeDef / USART_TypeDef.
 *   - This file is the BOARD layer: it is the ONLY place that knows the chip
 *     (stm32f4xx.h) and the real peripherals (ADC1, USART1, GPIOD). It builds
 *     the HAL handles via the per-platform hal _hal_create() helpers and hands
 *     them to the drivers. To move to another MCU you rewrite hal/<new-platform>/
 *     and this
 *     board file; the drv/ sources stay untouched.
 *
 * See moban/ for the OOC template (vtable + fun + create/destroy/init/deinit).
 *
 * After BIST the firmware enters a line-based command loop so a PC companion
 * test (tools/companion_test.py) can verify the TX/RX loopback:
 *   PING         -> PONG
 *   ECHO <text>  -> <text>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "stm32f4xx.h"           /* BOARD layer only: real peripherals (ADC1, USART1, GPIOD) */
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "iface/device.h"
#include "selftest.h"
/* HAL handles (opaque to the driver; created here in the board layer) */
#include "adc_hal.h"
#include "gpio_hal.h"
#include "uart_hal.h"
#include "temp_hal.h"            /* factory calib words — chip-specific, board reads them */

int main(void)
{
    /* --- BOARD layer: build HAL handles for the real silicon -------------- */
    adc_hal_handle_t  *h_adc  = adc_hal_create((void *)ADC1, 0);   /* PA0 = CH0 */
    gpio_hal_handle_t *h_led  = gpio_hal_create((void *)GPIOD, 12, 1); /* PD12 out */
    uart_hal_handle_t *h_uart = uart_hal_create((void *)USART1, 115200UL);

    /* 1. system clock -> 168 MHz (HSE -> PLL) */
    clock *clk = clock_create();

    /* 2. USART1 console on PA9/PA10 @ 115200 */
    uart *uart = uart_create(h_uart);
    uart_set_console(uart);

    /* 3. green LED (LD4) on PD12 as an output pin object */
    gpio_pin *led = gpio_pin_create(h_led);

    /* 3b. ADC1 on PA0 (channel 0); PA0 is the on-board blue button (pulled high) */
    adc *adc = adc_create(h_adc, 0);
    /* 3c. on-chip temperature sensor (ADC1_IN16), reuses the same ADC object
           through the unified `device *` interface — fully decoupled. The chip
           specific factory calib words are read by the board and passed in, so
           the temp_sensor driver stays free of any HAL / register access. */
    temp_sensor *temp = temp_sensor_create((device *)adc, 3300UL,
                                           temp_hal_ts_cal1(), temp_hal_ts_cal2());

    /* --- unified device handles -------------------------------------------
     * The upper layer holds a `device *` for EVERY driver and drives them all
     * through the identical virtual-dispatch API. This is the whole point of
     * the unified interface: the application code does not care which chip or
     * which concrete driver is behind the handle. */
    device *d_clk  = (device *)clk;
    device *d_uart = (device *)uart;
    device *d_led  = (device *)led;
    device *d_adc  = (device *)adc;
    device *d_temp = (device *)temp;

    /* every driver is brought up through the SAME virtual call */
    d_uart->vtable->open(d_uart);
    d_led->vtable->open(d_led);
    d_adc->vtable->open(d_adc);
    d_temp->vtable->open(d_temp);
    /* (clock is already configured in create(); re-running PLL config at
       runtime is skipped on purpose) */

    uint32_t hz = 0;
    d_clk->vtable->ioctl(d_clk, CLK_IOCTL_GET_SYSCLK_HZ, &hz);
    printf("Hello from STM32F407 Discovery (OOC)!\r\n");
    printf("System clock: %lu Hz, USART1 @ 115200 8N1\r\n",
           (unsigned long)hz);

    /* 4. on-board self-test (BIST) at boot */
    selftest *st = selftest_create(clk, uart, led, adc, temp);
    selftest_run(st);

    printf("READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP\r\n");

    /* 5. command loop (PC companion test exercises this) */
    char line[64];
    uint32_t idx = 0;
    while (1)
    {
        char c = uart->fun->getc(uart);        /* typed method via fun table */
        uart_console_putc(c);                  /* local echo for terminal use */

        if (c == '\r' || c == '\n')
        {
            if (idx > 0)
            {
                line[idx] = '\0';
                idx = 0;
                d_led->vtable->ioctl(d_led, GPIO_IOCTL_TOGGLE, NULL);   /* activity LED */

                if (strcmp(line, "PING") == 0)
                {
                    d_uart->vtable->write(d_uart, "PONG\r\n", 6);
                }
                else if (strncmp(line, "ECHO ", 5) == 0)
                {
                    char out[64];
                    int n = snprintf(out, sizeof(out), "%s\r\n", line + 5);
                    d_uart->vtable->write(d_uart, out, (size_t)n);
                }
                else if (strcmp(line, "BIST") == 0)
                {
                    selftest_run(st);        /* re-run self-test on demand */
                }
                else if (strncmp(line, "ADC", 3) == 0)
                {
                    uint32_t ch = 0;
                    if (line[3] == ' ')
                        ch = (uint32_t)atoi(line + 4);
                    if (ch > 18U) ch = 0U;

                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &ch);
                    uint32_t raw = 0;
                    d_adc->vtable->read(d_adc, &raw, sizeof(raw));
                    uint32_t mv = 0;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_READ_MV, &mv);
                    uint32_t zero = 0U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &zero); /* restore PA0 */

                    char out[64];
                    int n = snprintf(out, sizeof(out),
                                     "ADC CH%lu raw=%lu mV=%lu\r\n",
                                     (unsigned long)ch,
                                     (unsigned long)raw, (unsigned long)mv);
                    d_uart->vtable->write(d_uart, out, (size_t)n);
                }
                else if (strcmp(line, "TEMP") == 0)
                {
                    /* sample the temperature sensor (CH16) for display */
                    uint32_t traw = 0;
                    uint32_t ch = 16U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &ch);
                    d_adc->vtable->read(d_adc, &traw, sizeof(traw));
                    uint32_t zero = 0U;
                    d_adc->vtable->ioctl(d_adc, ADC_IOCTL_SET_CHANNEL, &zero);

                    int32_t t10 = 0;
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_READ_X10, &t10);
                    uint16_t cal1 = 0, cal2 = 0;
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_GET_CAL1, &cal1);
                    d_temp->vtable->ioctl(d_temp, TEMP_IOCTL_GET_CAL2, &cal2);
                    int32_t ip = t10 / 10;
                    int32_t fp = (t10 < 0) ? -(t10 % 10) : (t10 % 10);

                    char out[64];
                    int n = snprintf(out, sizeof(out),
                                     "TEMP raw=%lu cal1=%u cal2=%u C=%ld.%ld\r\n",
                                     (unsigned long)traw,
                                     (unsigned)cal1,
                                     (unsigned)cal2,
                                     (long)ip, (long)fp);
                    d_uart->vtable->write(d_uart, out, (size_t)n);
                }
                else
                {
                    d_uart->vtable->write(d_uart, "ERR unknown\r\n", 13);
                }
            }
        }
        else if (idx < (sizeof(line) - 1))
        {
            line[idx++] = c;
        }
    }
}
