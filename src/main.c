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
 * LAYERING (fully decoupled):
 *   - drv/        : platform-independent drivers, hold only OPAQUE HAL handles.
 *   - hal/stm32/  : the ONLY place that touches chip registers (ADC_TypeDef ...).
 *   - board/      : the ONLY place that knows the real peripherals (ADC1,
 *                   USART1, GPIOD) — expressed as const DATA + a construction
 *                   loop. Equivalent to a device tree + board init.
 *   - devmgr/     : generic name -> device* registry (device_get_binding style).
 *   - main.c      : APPLICATION layer. It knows device NAMES only
 *                   (device_manager_get("uart0")); it never sees a peripheral
 *                   base address or a HAL handle, and includes no chip header.
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
#include "iface/device.h"
#include "devmgr/device_manager.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "selftest.h"

int main(void)
{
    /* BOARD layer builds HAL handles + drivers from its descriptor and
     * registers them by name. This file learns nothing about the silicon. */
    board_init();

    /* --- unified device handles (by NAME, not by peripheral) -------------
     * The application holds a `device *` for every driver and drives them all
     * through the identical virtual-dispatch API. It does not care which chip
     * or which concrete driver is behind each name. */
    device *d_clk  = device_manager_get("clk");
    device *d_uart = device_manager_get("uart0");
    device *d_led  = device_manager_get("led");
    device *d_adc  = device_manager_get("adc0");
    device *d_temp = device_manager_get("temp0");

    /* every driver is brought up through the SAME virtual call.
     * create() only builds the object; hardware is started here in open(). */
    d_clk->vtable->open(d_clk);      /* configure the PLL */
    d_uart->vtable->open(d_uart);
    d_led->vtable->open(d_led);
    d_adc->vtable->open(d_adc);
    d_temp->vtable->open(d_temp);

    uint32_t hz = 0;
    d_clk->vtable->ioctl(d_clk, CLK_IOCTL_GET_SYSCLK_HZ, &hz);
    printf("Hello from STM32F407 Discovery (OOC)!\r\n");
    printf("System clock: %lu Hz, USART1 @ 115200 8N1\r\n",
           (unsigned long)hz);

    /* 4. on-board self-test (BIST) at boot.
     * selftest_create takes the unified device* handles from the registry. */
    selftest *st = selftest_create(d_clk, d_uart, d_led, d_adc, d_temp);
    selftest_run(st);

    printf("READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP\r\n");

    /* 5. command loop (PC companion test exercises this) */
    char line[64];
    uint32_t idx = 0;
    while (1)
    {
        char c = 0;
        if (d_uart->vtable->read(d_uart, &c, 1) != 1)
            continue;                    /* no char available (HW read blocks) */
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
