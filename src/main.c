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
#include "log/log.h"
#include "log/app_log.h"
#include <string.h>
#include <stdlib.h>
#include "iface/device.h"
#include "iface/stream_device.h"   /* device_as_stream downcast */
#include "iface/io_xfer.h"         /* io_xfer_t, io_xfer_complete */
#include "devmgr/device_manager.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/pinmux.h"
#include "drv/usb.h"
#include "drv/i2c.h"
#include "selftest.h"

/* completion callback for the IOXFER async demo: records that the transfer
 * finished. Runs in ISR/thread context depending on the engine; just sets a
 * flag so it stays ISR-safe. */
static void io_demo_cb(io_xfer_t *x)
{
    if (x && x->arg)
        *(int *)x->arg = 1;
}

int main(void)
{
    /* BOARD layer builds HAL handles + drivers from its descriptor and
     * registers them by name. This file learns nothing about the silicon. */
    board_init();

    /* Start the board's 1 kHz SysTick tick service (registered through the
     * platform-independent irq framework) — demonstrates a core exception
     * dispatched by the same mechanism as device IRQs. */
    board_tick_init();

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
    log_printf(app_log(), LOG_INFO, "main", "Hello from STM32F407 Discovery (OOC)!\n");
    log_printf(app_log(), LOG_INFO, "main", "System clock: %lu Hz, USART1 @ 115200 8N1\n",
           (unsigned long)hz);

    /* Confirmation marker: proves the name-based pinmux changes (board supplies
     * signal NAMES like "USART1_TX_PA9", drivers resolve via pinmux_hal_resolve)
     * are compiled in AND flashed. __DATE__/__TIME__ make every build unique so
     * we can tell a fresh image from a stale one on the board. */
    log_printf(app_log(), LOG_INFO, "main", "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
           __DATE__, __TIME__);

    /* 4. on-board self-test (BIST) at boot.
     * selftest_create takes the unified device* handles from the registry. */
    selftest *st = selftest_create(d_clk, d_uart, d_led, d_adc, d_temp);
    selftest_run(st);

    /* pinmux conflict-detection self-test (exercises the new driver) */
    device *d_pinmux = device_manager_get("pinmux");
    int pmok = pinmux_run_selftest((pinmux *)d_pinmux);
    log_printf(app_log(), LOG_INFO, "main", "[BIST] pinmux: %s\n", pmok ? "PASS" : "FAIL");

    /* Bring up the CDC device and LEAVE it connected so a real PC host can
     * enumerate it automatically at boot (VID_0483&PID_5740). The BIST opens
     * then closes usb0; we re-open it here and never close it, so the device
     * stays enumerated. (The USBOPEN/USBCLOSE console commands still work.) */
    device *d_usb = device_manager_get("usb0");
    if (!d_usb) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: NOT REGISTERED\n");
    } else if (d_usb->vtable->open(d_usb)) {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: OPEN FAILED\n");
    } else {
        log_printf(app_log(), LOG_INFO, "main", "[boot] usb0: connected (CDC ACM, VID_0483 PID_5740)\n");
    }

    log_printf(app_log(), LOG_INFO, "main", "READY. Commands: PING / ECHO <text> / BIST / ADC [ch] / TEMP / TICKS / I2C_IRQ / USBOPEN / USBCLOSE / USBSTAT / USBDBG [0|1]\n");

    /* NOTE: d_usb (usb0) is already declared/opened just above and stays in
     * scope for the loop below, where we use it for the CDC loopback echo. */

    /* 5. command loop (PC companion test exercises this) */
    char line[64];
    uint32_t idx = 0;
    while (1)
    {
        /* --- USB CDC loopback echo -----------------------------------------
         * Poll the CDC bulk-OUT ring buffer and echo every received byte back
         * on the bulk-IN endpoint. This proves BOTH directions end-to-end:
         *  - RX: we received exactly what the PC sent (it lands in the rb)
         *  - TX: the PC receives exactly what we wrote on EP 0x81
         * usb_stream_write() now stages into a TX ring and returns immediately
         * (the IN endpoint is armed by usb_tx_pump from the main loop and the
         * IN-complete ISR), so this never blocks the loop. Runs before the UART
         * read so it is serviced even when UART is idle.
         * (CDC baud is virtual; the real ceiling is USB FS bulk bandwidth.) */
        if (d_usb) {
            static uint8_t ub[64];
            /* Cap the read to what the TX staging ring can accept right now, so
             * we never pull more from RX than we can echo (no silent drops). */
            size_t tx_free = sizeof(ub);
            d_usb->vtable->ioctl(d_usb, USB_IOCTL_TX_FREE, &tx_free);
            size_t want = tx_free < sizeof(ub) ? tx_free : sizeof(ub);
            int n = 0;
            if (want) n = d_usb->vtable->read(d_usb, ub, want);
            if (n > 0)
                d_usb->vtable->write(d_usb, ub, (size_t)n);   /* echo -> PC */
            /* Drain the TX staging ring into the bulk-IN endpoint. This is the
             * ring's ONLY consumer (the IN-complete ISR only clears the busy
             * flag), so it is safe to call here every iteration — including
             * when no new RX arrived but data is still staged. */
            d_usb->vtable->ioctl(d_usb, USB_IOCTL_TX_PUMP, NULL);
            /* Re-arm the bulk-OUT endpoint if it was NAK'd because the RX ring
             * had no room (USB back-pressure). Called after the above drained
             * RX, so room is available again. */
            d_usb->vtable->ioctl(d_usb, USB_IOCTL_RX_REARM, NULL);
        }

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
                    /* re-emit the build marker on demand so a PC companion that
                     * connects AFTER boot can still confirm which image is flashed */
                    log_printf(app_log(), LOG_INFO, "main", "BUILD: pinmux name-based (USART1_TX_PA9 / GPIOD_12 / ADC1_IN0) - %s %s\n",
                           __DATE__, __TIME__);
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
                else if (strcmp(line, "I2C_IRQ") == 0)
                {
                    device *i2cd = device_manager_get("i2c0");
                    if (!i2cd) { log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: no dev\n"); }
                    else if (i2cd->vtable->open(i2cd)) { log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: open FAIL\n"); }
                    else {
                        stream_xfer_mode_t irq_m = STREAM_MODE_IRQ;
                        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
                        uart_console_putc('a'); uart_console_putc('\n');
                        i2c_xfer_t ip = { .addr = 0x50, .buf = NULL, .len = 0, .result = 0 };
                        int r = i2cd->vtable->ioctl(i2cd, I2C_IOCTL_MASTER_WRITE, &ip);
                        uart_console_putc('b'); uart_console_putc('\n');
                        log_printf(app_log(), LOG_INFO, "main", "I2C_IRQ: result=%d probe=%s\n", r, ip.result == -1 ? "NACK" : "ERR");
                        irq_m = STREAM_MODE_POLL;
                        i2cd->vtable->ioctl(i2cd, STREAM_IOCTL_SET_MODE, &irq_m);
                        i2cd->vtable->close(i2cd);
                    }
                }
                else if (strcmp(line, "TICKS") == 0)
                {
                    /* prove the platform-independent irq framework is live:
                     * the SysTick ISR increments g_ticks via irq_dispatch(). */
                    char out[32];
                    int n = snprintf(out, sizeof(out), "TICKS %lu\r\n",
                                     (unsigned long)board_ticks());
                    d_uart->vtable->write(d_uart, out, (size_t)n);
                }
                else if (strcmp(line, "USBOPEN") == 0)
                {
                    /* Bring up the CDC device and LEAVE it connected so a real PC
                     * host can enumerate it (VID_0483&PID_5740). The BIST opens then
                     * closes usb0, which disconnects it — so for real enumeration the
                     * device must stay open. Plug the CN5 cable into the PC first. */
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { d_uart->vtable->write(d_uart, "USBOPEN: no dev\r\n", 18); }
                    else if (usbd->vtable->open(usbd)) {
                        d_uart->vtable->write(d_uart, "USBOPEN: open FAIL\r\n", 20);
                    } else {
                        d_uart->vtable->write(d_uart,
                            "USBOPEN: usb0 connected (plug CN5 into PC)\r\n", 43);
                    }
                }
                else if (strcmp(line, "USBCLOSE") == 0)
                {
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { d_uart->vtable->write(d_uart, "USBCLOSE: no dev\r\n", 19); }
                    else { usbd->vtable->close(usbd);
                           d_uart->vtable->write(d_uart, "USBCLOSE: usb0 off\r\n", 20); }
                }
                else if (strcmp(line, "USBSTAT") == 0)
                {
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { d_uart->vtable->write(d_uart, "USBSTAT: no dev\r\n", 18); }
                    else { usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_DUMP, NULL); }
                }
                else if (strncmp(line, "USBDBG", 6) == 0)
                {
                    /* Toggle ISR trace (SETUP/OUT/IN decoding) for live debugging.
                     * NOTE: printing inside the USB ISR perturbs timing, so leave
                     * it OFF for normal enumeration; turn on only to watch a host
                     * that is already enumerating.  USBDBG 1 = on, USBDBG 0 = off. */
                    int on = 0;
                    if (line[6] == ' ') on = atoi(line + 7);
                    device *usbd = device_manager_get("usb0");
                    if (!usbd) { d_uart->vtable->write(d_uart, "USBDBG: no dev\r\n", 17); }
                    else {
                        usbd->vtable->ioctl(usbd, USB_IOCTL_DBG_SET, &on);
                        d_uart->vtable->write(d_uart,
                            on ? "USBDBG: trace ON\r\n" : "USBDBG: trace OFF\r\n",
                            on ? 17 : 18);
                    }
                }
                else if (strcmp(line, "IOXFER") == 0)
                {
                    /* Demo the unified sync/async transfer API (the framework
                     * top-level over stream_device). Try it: type IOXFER and the
                     * board sends two lines through stream_device_transfer_sync / _async.
                     * The async path uses a completion callback (cb) that sets a
                     * flag, proving the ISR/callback plumbing end-to-end. */
                    stream_device *s = device_as_stream(d_uart);
                    if (!s) {
                        d_uart->vtable->write(d_uart, "ERR no stream\r\n", 14);
                    } else {
                        static int g_io_cb_fired;
                        g_io_cb_fired = 0;

                        const char *m1 = "[IOXFER] sync transfer\r\n";
                        io_xfer_t sx = { .buf = (void *)m1, .len = strlen(m1),
                                         .dir = IO_XFER_DIR_WRITE };
                        int rs = stream_device_transfer_sync(s, &sx);

                        const char *m2 = "[IOXFER] async transfer (cb)\r\n";
                        io_xfer_t ax = { .buf = (void *)m2, .len = strlen(m2),
                                         .dir = IO_XFER_DIR_WRITE,
                                         .callback = io_demo_cb,
                                         .arg = &g_io_cb_fired };
                        int ra = stream_device_transfer_async(s, &ax);

                        char out[64];
                        int n = snprintf(out, sizeof(out),
                                         "[IOXFER] sync r=%d done=%d | async r=%d (started)\r\n",
                                         rs, (int)sx.done, ra);
                        d_uart->vtable->write(d_uart, out, (size_t)n);

                        /* The async transfer completes in the TXE ISR. The
                         * blocking write just above can only proceed once the
                         * line is free — i.e. AFTER the async transfer finished —
                         * so by the time it returned, ax.done and the callback
                         * flag are final. Print them now to prove the interrupt
                         * path + completion callback fired end-to-end. */
                        n = snprintf(out, sizeof(out),
                                     "[IOXFER] async done=%d cb=%d\r\n",
                                     (int)ax.done, g_io_cb_fired);
                        d_uart->vtable->write(d_uart, out, (size_t)n);
                    }
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
