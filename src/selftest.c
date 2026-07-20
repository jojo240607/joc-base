#include "selftest.h"
#include "irq_manager.h"     /* dump the centralized interrupt registry in BIST */
#include "stm32f4xx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* IOCTL command constants for the unified device interface (just ints) */
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/timer.h"
#include "devmgr/device_manager.h"
#include "iface/stream_device.h"   /* device_as_stream downcast */
#include "iface/io_xfer.h"         /* io_xfer_t, io_xfer_complete */
#include "common/ringbuffer.h"     /* ringbuffer_run_selftest (common utility class) */

#define UART_PCLK2_HZ 84000000UL
#define UART_BAUD     115200UL

static int selftest_vclock(selftest *self);
static int selftest_vuart(selftest *self);
static int selftest_vgpio(selftest *self);
static int selftest_vadc(selftest *self);
static int selftest_vtemp(selftest *self);
static int selftest_vio(selftest *self);
static int selftest_vmode(selftest *self);
static int selftest_vtimer(selftest *self);

/* one shared vtable for the whole self-test class */
static const struct selftestVtable selftest_vtable = {
    .test_clock = selftest_vclock,
    .test_uart  = selftest_vuart,
    .test_gpio  = selftest_vgpio,
    .test_adc   = selftest_vadc,
    .test_temp  = selftest_vtemp,
    .test_io    = selftest_vio,
    .test_mode  = selftest_vmode,
    .test_timer = selftest_vtimer,
};

const struct selftestFun selftest_fun = {
    .destroy = selftest_destroy,
    .init = selftest_init,
    .deinit = selftest_deinit,
    .run = selftest_run,
};

selftest *selftest_create(device *clk, device *uart, device *led,
                           device *adc, device *temp)
{
    selftest *self = (selftest *)malloc(sizeof(selftest));
    if (!self) return NULL;
    memset(self, 0, sizeof(selftest));
    self->clk  = clk;
    self->uart = uart;
    self->led  = led;
    self->adc  = adc;
    self->temp = temp;
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
    self->vtable = &selftest_vtable;   /* per-class shared vtable */
    self->fun = &selftest_fun;
}

void selftest_deinit(selftest *self)
{
    if (!self) return;
    /* no vtable to free (it is per-class static const) */
}

int selftest_run(selftest *self)
{
    if (!self || !self->vtable) return 0;

    int pass = 1;
    int r;

    printf("\r\n--- On-board self-test (BIST) ---\r\n");

    irq_manager_dump();   /* dump the centralized interrupt registry (verify wiring) */

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

    r = self->vtable->test_temp(self);
    printf("[BIST] temp  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_io(self);
    printf("[BIST] io    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_mode(self);
    printf("[BIST] mode  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_timer(self);
    printf("[BIST] timer : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    /* ring buffer utility class (common/) — exercised standalone so the class
     * itself is proven independent of any driver. */
    r = ringbuffer_run_selftest();
    printf("[BIST] ringbuf: %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    printf("SELF-TEST: %s\r\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* --- sub-tests (virtual) — all driven through the unified device interface --- */

static int selftest_vclock(selftest *self)
{
    uint32_t hz = 0;
    self->clk->vtable->ioctl(self->clk, CLK_IOCTL_GET_SYSCLK_HZ, &hz);

    int pll_locked = (RCC->CR & RCC_CR_PLLRDY) != 0;
    int pll_selected = ((RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL);
    int hz_ok = (hz == 168000000UL);
    return pll_locked && pll_selected && hz_ok;
}

static int selftest_vuart(selftest *self)
{
    /* Verify the USART1 peripheral is enabled (TX+RX) and BRR is correct.
       We intentionally do NOT transmit here so the serial wire stays clean
       for the PC companion test (the RX/TX loopback is checked there). */
    uint32_t cr1 = 0, brr = 0;
    self->uart->vtable->ioctl(self->uart, UART_IOCTL_GET_CR1, &cr1);
    self->uart->vtable->ioctl(self->uart, UART_IOCTL_GET_BRR, &brr);

    int enabled = (cr1 & (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE))
                  == (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
    int br_ok = (brr == (uint32_t)(UART_PCLK2_HZ / UART_BAUD));
    return enabled && br_ok;
}

static int selftest_vgpio(selftest *self)
{
    uint8_t before = 0, after = 0;
    self->led->vtable->read(self->led, &before, 1);
    self->led->vtable->ioctl(self->led, GPIO_IOCTL_TOGGLE, NULL);
    self->led->vtable->read(self->led, &after, 1);
    if (before != after)
        self->led->vtable->ioctl(self->led, GPIO_IOCTL_TOGGLE, NULL);   /* restore level */
    return before != after;
}

static int selftest_vadc(selftest *self)
{
    device *adc = self->adc;
    if (!adc) return 0;

    /* Read the internal VREFINT (channel 17, ~1.21 V). This exercises the
       ADC clock, sequence programming, EOC polling and data read path with
       no external wiring. At VDDA~3.3V a 12-bit reading lands near 1500;
       accept a wide sane band in case VDDA differs. */
    uint32_t saved = 0;
    adc->vtable->ioctl(adc, ADC_IOCTL_GET_CHANNEL, &saved);
    uint32_t ch = 17U;
    adc->vtable->ioctl(adc, ADC_IOCTL_SET_CHANNEL, &ch);
    uint32_t vref = 0;
    adc->vtable->read(adc, &vref, sizeof(vref));
    uint32_t zero = 0U;
    adc->vtable->ioctl(adc, ADC_IOCTL_SET_CHANNEL, &zero);   /* restore external channel */

    int vref_ok = (vref >= 800U && vref <= 2200U);
    printf("       VREFINT raw=%lu (expect ~1500) %s\r\n",
           (unsigned long)vref, vref_ok ? "" : "[OUT OF RANGE]");
    return vref_ok;
}

static int selftest_vtemp(selftest *self)
{
    device *temp = self->temp;
    if (!temp) return 0;

    int32_t t10 = 0;
    uint16_t cal1 = 0, cal2 = 0;
    temp->vtable->ioctl(temp, TEMP_IOCTL_READ_X10, &t10);
    temp->vtable->ioctl(temp, TEMP_IOCTL_GET_CAL1, &cal1);
    temp->vtable->ioctl(temp, TEMP_IOCTL_GET_CAL2, &cal2);

    int32_t ip = t10 / 10;
    int32_t fp = (t10 < 0) ? -(t10 % 10) : (t10 % 10);
    printf("       die temp = %ld.%ld C (cal1=%u cal2=%u)\r\n",
           (long)ip, (long)fp, (unsigned)cal1, (unsigned)cal2);

    /* A running STM32 die is typically 10..90 C; accept a wide band so the
       test is robust to ambient/self-heating, while still rejecting garbage. */
    int temp_ok = (t10 > -200 && t10 < 1200);   /* -20.0 C .. 120.0 C */
    return temp_ok;
}

/* Verify the unified sync/async transfer API wiring WITHOUT putting any bytes
 * on the wire (keeps the serial clean for the PC companion test). We check:
 *   - the stream downcast (device_as_stream) resolves for uart and adc;
 *   - stream_device_transfer_async is REJECTED (-1) for a driver without submit (adc);
 *   - io_transfer_async is ACCEPTED (0) for a driver with submit (uart).
 * The real transmit/receive paths are exercised interactively via the IOXFER
 * command in main.c. */
static int selftest_vio(selftest *self)
{
    stream_device *us = device_as_stream(self->uart);
    stream_device *as = device_as_stream(self->adc);
    if (!us || !as) return 0;

    char dummy[1];
    io_xfer_t no_submit = { .buf = dummy, .len = 0, .dir = IO_XFER_DIR_WRITE };
    int rejected = (stream_device_transfer_async(as, &no_submit) == -1);   /* adc: no submit */

    io_xfer_t has_submit = { .buf = dummy, .len = 0, .dir = IO_XFER_DIR_WRITE };
    int accepted = (stream_device_transfer_async(us, &has_submit) == 0);   /* uart: has submit */

    printf("       downcast ok, async reject(adc)=%s accept(uart)=%s\r\n",
           rejected ? "yes" : "NO", accepted ? "yes" : "NO");
    return rejected && accepted;
}

/* Verify the POLL/IRQ engine switching is real (not just a declared ioctl) and
 * that the ADC interrupt path actually works end-to-end:
 *   - switch ADC to STREAM_MODE_IRQ, do a blocking read (driven by the EOC ISR),
 *     and confirm a sane 12-bit value comes back; restore POLL afterwards.
 *   - switch UART POLL<->IRQ via STREAM_IOCTL_SET_MODE and confirm GET_MODE
 *     round-trips. (No wire traffic: reads/writes here are to the device's own
 *     registers / mode field, not the terminal.) */
static int selftest_vmode(selftest *self)
{
    device *adc  = self->adc;
    device *uart = self->uart;
    int ok = 1;

    /* ADC: interrupt-mode (EOC ISR) read must return a sane 12-bit value. */
    stream_xfer_mode_t m = STREAM_MODE_IRQ;
    adc->vtable->ioctl(adc, STREAM_IOCTL_SET_MODE, &m);
    uint32_t adc_irq = 0;
    int nr = adc->vtable->read(adc, &adc_irq, sizeof(adc_irq));
    if (nr != (int)sizeof(adc_irq) || adc_irq > 4095U) ok = 0;
    m = STREAM_MODE_POLL;                       /* restore polling default */
    adc->vtable->ioctl(adc, STREAM_IOCTL_SET_MODE, &m);

    /* UART: mode switching roundtrip (POLL <-> IRQ). */
    m = STREAM_MODE_POLL;
    uart->vtable->ioctl(uart, STREAM_IOCTL_SET_MODE, &m);
    stream_xfer_mode_t got = STREAM_MODE_IRQ;
    uart->vtable->ioctl(uart, STREAM_IOCTL_GET_MODE, &got);
    if (got != STREAM_MODE_POLL) ok = 0;
    m = STREAM_MODE_IRQ;
    uart->vtable->ioctl(uart, STREAM_IOCTL_SET_MODE, &m);
    uart->vtable->ioctl(uart, STREAM_IOCTL_GET_MODE, &got);
    if (got != STREAM_MODE_IRQ) ok = 0;

    printf("       adc irq read=%lu, uart mode switch %s\r\n",
           (unsigned long)adc_irq, ok ? "ok" : "FAIL");
    return ok;
}

/* Verify the general-purpose TIM driver works end-to-end as a periodic EVENT
 * source: open timer0, register a TICK callback, enable it (start counting +
 * arm NVIC), busy-wait a few hundred ms, then confirm BOTH that the overflow
 * ISR actually fired (overflows > 0) AND that the registered callback was
 * invoked (cb_count > 0). timer0 is configured at 20 Hz, so a ~300 ms wait
 * should yield several overflows/callbacks. No external wiring. */
static volatile uint32_t g_timer_cb_count;   /* bumped from ISR context */
static void selftest_timer_cb(void *ctx, device_event_type_t ev, void *ev_data)
{
    (void)ctx; (void)ev; (void)ev_data;
    g_timer_cb_count++;
}
static int selftest_vtimer(selftest *self)
{
    (void)self;
    device *tim = device_manager_get("timer0");
    if (!tim) return 0;
    event_device *te = device_as_event(tim);
    if (!te) return 0;

    g_timer_cb_count = 0;
    te->vtable->set_event_callback(te, DEVICE_EVENT_TICK, selftest_timer_cb, NULL);
    tim->vtable->open(tim);
    te->vtable->enable(te);          /* start counting + arm NVIC */

    /* busy-wait ~300 ms (timer is 20 Hz => expect a handful of overflows) */
    for (volatile uint32_t i = 0; i < (SystemCoreClock / 10U); i++) { }

    uint32_t ov = 0;
    tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov);
    te->vtable->disable(te);
    te->vtable->clear_event_callback(te, DEVICE_EVENT_TICK);
    tim->vtable->close(tim);

    int ok_isr   = (ov >= 2U);
    int ok_cb    = (g_timer_cb_count >= 2U);
    int ok       = ok_isr && ok_cb;
    printf("       timer0 overflows=%lu (ISR %s), cb_count=%lu (callback %s)\r\n",
           (unsigned long)ov, ok_isr ? "PASS" : "FAIL",
           (unsigned long)g_timer_cb_count, ok_cb ? "PASS" : "FAIL");
    return ok;
}
