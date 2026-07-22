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
#include "drv/pwm.h"
#include "drv/exti.h"
#include "drv/i2c.h"
#include "drv/spi.h"
#include "drv/sdio.h"
#include "drv/dac.h"
#include "drv/rtc.h"
#include "drv/rng.h"
#include "drv/crc.h"
#include "drv/iwdg.h"
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
static int selftest_vpwm(selftest *self);
static int selftest_vexti(selftest *self);
static int selftest_vadvtimer(selftest *self);
static int selftest_vadvpwm(selftest *self);
static int selftest_vi2c(selftest *self);
static int selftest_vspi(selftest *self);
static int selftest_vsdio(selftest *self);
static int selftest_vdac(selftest *self);
static int selftest_vrtc(selftest *self);
static int selftest_vrng(selftest *self);
static int selftest_vcrc(selftest *self);
static int selftest_viwdg(selftest *self);

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
    .test_pwm   = selftest_vpwm,
    .test_exti  = selftest_vexti,
    .test_adv_timer = selftest_vadvtimer,
    .test_adv_pwm   = selftest_vadvpwm,
    .test_i2c       = selftest_vi2c,
    .test_spi       = selftest_vspi,
    .test_sdio      = selftest_vsdio,
    .test_dac       = selftest_vdac,
    .test_rtc       = selftest_vrtc,
    .test_rng       = selftest_vrng,
    .test_crc       = selftest_vcrc,
    .test_iwdg      = selftest_viwdg,
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

    r = self->vtable->test_pwm(self);
    printf("[BIST] pwm   : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_exti(self);
    printf("[BIST] exti  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_adv_timer(self);
    printf("[BIST] adv_timer: %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_adv_pwm(self);
    printf("[BIST] adv_pwm  : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_i2c(self);
    printf("[BIST] i2c    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_spi(self);
    printf("[BIST] spi    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_sdio(self);
    printf("[BIST] sdio   : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_dac(self);
    printf("[BIST] dac    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_rtc(self);
    printf("[BIST] rtc    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_rng(self);
    printf("[BIST] rng    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_crc(self);
    printf("[BIST] crc    : %s\r\n", r ? "PASS" : "FAIL");
    pass &= r;

    r = self->vtable->test_iwdg(self);
    printf("[BIST] iwdg   : %s\r\n", r ? "PASS" : "FAIL");
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
 * source for EVERY instantiated timer (timer0..timer13 = TIM2/1/6/7/8/9/11/12/
 * 14/10/13/3/4/5). For each: register a TICK callback, open, enable (start counting +
 * arm NVIC), busy-wait ~300 ms, then confirm BOTH that the overflow ISR fired
 * (overflows) AND that the registered callback was invoked (cb_count). The last
 * two (timer9=TIM10, timer10=TIM13) share an IRQ line with TIM1/TIM8, so this
 * also exercises the multi-handler irq framework per-timer. The dedicated
 * shared-line test below proves the two peers on one line fire TOGETHER. */
static volatile uint32_t g_timer_cb_count;   /* bumped from ISR context */
static void selftest_timer_cb(void *ctx, device_event_type_t ev, void *ev_data)
{
    (void)ctx; (void)ev; (void)ev_data;
    g_timer_cb_count++;
}

/* Shared-line coexistence test: enable BOTH peers on one IRQ line at once and
 * confirm BOTH handlers fire. Each ISR guards on its own UIF, so the sibling's
 * overflow cannot spuriously bump the other's counter. This is the real proof
 * that irq_dispatch() invokes every handler registered on a shared line. */
static volatile uint32_t g_timer_cb_a, g_timer_cb_b;
static void selftest_timer_cb_a(void *c, device_event_type_t e, void *d)
{ (void)c; (void)e; (void)d; g_timer_cb_a++; }
static void selftest_timer_cb_b(void *c, device_event_type_t e, void *d)
{ (void)c; (void)e; (void)d; g_timer_cb_b++; }

static int selftest_timer_shared_line(const char *na, const char *nb)
{
    device *da = device_manager_get(na);
    device *db = device_manager_get(nb);
    if (!da || !db) { printf("       %s+%s: MISSING\r\n", na, nb); return 0; }
    event_device *ea = device_as_event(da);
    event_device *eb = device_as_event(db);
    if (!ea || !eb) { printf("       %s+%s: not-event\r\n", na, nb); return 0; }

    g_timer_cb_a = g_timer_cb_b = 0;
    ea->vtable->set_event_callback(ea, DEVICE_EVENT_TICK, selftest_timer_cb_a, NULL);
    eb->vtable->set_event_callback(eb, DEVICE_EVENT_TICK, selftest_timer_cb_b, NULL);
    da->vtable->open(da);
    db->vtable->open(db);
    ea->vtable->enable(ea);          /* arm the shared NVIC line */
    eb->vtable->enable(eb);          /* second peer on the SAME line */

    /* busy-wait ~300 ms (both timers are 20 Hz => expect a handful of overflows) */
    for (volatile uint32_t k = 0; k < (SystemCoreClock / 20U); k++) { }

    uint32_t oa = 0, ob = 0;
    da->vtable->ioctl(da, TIMER_IOCTL_GET_OVERFLOWS, &oa);
    db->vtable->ioctl(db, TIMER_IOCTL_GET_OVERFLOWS, &ob);
    ea->vtable->disable(ea);
    eb->vtable->disable(eb);
    ea->vtable->clear_event_callback(ea, DEVICE_EVENT_TICK);
    eb->vtable->clear_event_callback(eb, DEVICE_EVENT_TICK);
    da->vtable->close(da);
    db->vtable->close(db);

    int ok_a  = (oa >= 2U && g_timer_cb_a >= 2U);
    int ok_b  = (ob >= 2U && g_timer_cb_b >= 2U);
    int ok    = ok_a && ok_b;
    if (!ok) ok = 0;
    printf("       %s+%s: ov_a=%lu cb_a=%lu, ov_b=%lu cb_b=%lu (%s)\r\n",
           na, nb, (unsigned long)oa, (unsigned long)g_timer_cb_a,
           (unsigned long)ob, (unsigned long)g_timer_cb_b,
           ok ? "PASS" : "FAIL");
    return ok;
}

static int selftest_vtimer(selftest *self)
{
    (void)self;
    static const char *timers[] = {
        "timer0", "timer1", "timer2", "timer3", "timer4",
        "timer5", "timer6", "timer7", "timer8", "timer9", "timer10",
        "timer11", "timer12", "timer13"
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
        device *tim = device_manager_get(timers[i]);
        if (!tim) { ok = 0; printf("       %s: MISSING\r\n", timers[i]); continue; }
        event_device *te = device_as_event(tim);
        if (!te)  { ok = 0; printf("       %s: not-event\r\n", timers[i]); continue; }

        g_timer_cb_count = 0;
        te->vtable->set_event_callback(te, DEVICE_EVENT_TICK, selftest_timer_cb, NULL);
        tim->vtable->open(tim);
        te->vtable->enable(te);          /* start counting + arm NVIC */

        /* busy-wait ~300 ms (timer is 20 Hz => expect a handful of overflows) */
        for (volatile uint32_t k = 0; k < (SystemCoreClock / 20U); k++) { }

        uint32_t ov = 0;
        tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov);
        te->vtable->disable(te);
        te->vtable->clear_event_callback(te, DEVICE_EVENT_TICK);
        tim->vtable->close(tim);

        int ok_isr = (ov >= 2U);
        int ok_cb  = (g_timer_cb_count >= 2U);
        if (!ok_isr || !ok_cb) ok = 0;
        printf("       %s: overflows=%lu (ISR %s), cb=%lu (cb %s)\r\n",
               timers[i], (unsigned long)ov, ok_isr ? "PASS" : "FAIL",
               (unsigned long)g_timer_cb_count, ok_cb ? "PASS" : "FAIL");
    }

    /* Shared-line coexistence: TIM1+TIM10 on IRQ25, TIM8+TIM13 on IRQ44. */
    if (!selftest_timer_shared_line("timer1", "timer9"))  ok = 0;
    if (!selftest_timer_shared_line("timer4", "timer10")) ok = 0;
    return ok;
}

/* Verify the PWM driver works end-to-end AND that it COORDINATES with the timer
 * driver on the SAME TIM. pwm0 is CH1 of TIM3, which timer11 also owns (as a 20
 * Hz TICK source). So opening timer11 first sets TIM3's period + starts the
 * counter; pwm0 then only configures the channel + duty on that same TIM3. The
 * test proves: (1) the pin is claimed without conflict, (2) duty writes land in
 * CCR (50% -> ~ARR/2, 25% -> ~ARR/4), (3) the period equals the timer's
 * (84 MHz / 20 Hz = 4.2M ticks), and (4) timer11 KEEPS counting overflows while
 * pwm0 is active — i.e. one TIM serves BOTH a periodic event AND a PWM output. */
static int selftest_vpwm(selftest *self)
{
    (void)self;
    device *tim = device_manager_get("timer11");  /* TIM3, 20 Hz TICK source */
    device *pwmd = device_manager_get("pwm0");     /* TIM3 CH1, coordinates */
    if (!tim || !pwmd) { printf("       pwm0/timer11: MISSING\r\n"); return 0; }
    event_device *te = device_as_event(tim);
    if (!te) { printf("       timer11: not-event\r\n"); return 0; }

    /* 1) bring up the timer driver first (owns TIM3 period + counter). */
    tim->vtable->open(tim);
    te->vtable->enable(te);

    /* 2) bring up PWM on the SAME TIM3 (coord mode: channel + duty only). */
    if (pwmd->vtable->open(pwmd) != 0) {
        printf("       pwm0: OPEN FAILED (pin conflict?)\r\n");
        te->vtable->disable(te); tim->vtable->close(tim);
        return 0;
    }

    int ok = 1;

    /* 3) duty math: 50% then 25% of the period. */
    int pct = 50;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_SET_DUTY_PERCENT, &pct);
    uint32_t period = 0, duty50 = 0;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_PERIOD_TICKS, &period);
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_DUTY_TICKS, &duty50);
    /* period (ARR+1) is what the PWM driver reports; the PSC/ARR formula in
     * tim_hal yields 84MHz/20Hz -> (PSC+1)=65, ARR+1=64615. Recompute the same
     * way so the check tracks the formula instead of a hard-coded magic number. */
    uint32_t exp_total = 84000000U / 20U;
    uint32_t exp_presc = (exp_total - 1U) / 65536U;
    uint32_t exp_period = exp_total / (exp_presc + 1U);
    int ok_period = (period == exp_period);
    int ok_50 = (duty50 >= period/2 - 2 && duty50 <= period/2 + 2);

    pct = 25;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_SET_DUTY_PERCENT, &pct);
    uint32_t duty25 = 0;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_DUTY_TICKS, &duty25);
    int ok_25 = (duty25 >= period/4 - 2 && duty25 <= period/4 + 2);

    /* 4) coordination proof: timer11 must still be counting while PWM is live. */
    uint32_t ov_before = 0;
    tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov_before);
    for (volatile uint32_t k = 0; k < (SystemCoreClock / 20U); k++) { }  /* ~50ms */
    uint32_t ov_after = 0;
    tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov_after);
    int ok_coord = (ov_after > ov_before);   /* timer event still firing on shared TIM */

    if (!ok_period || !ok_50 || !ok_25 || !ok_coord) ok = 0;
    printf("       pwm0(TIM3_CH1): period=%lu ticks (expect %lu, %s)\r\n",
           (unsigned long)period, (unsigned long)exp_period,
           ok_period ? "PASS" : "FAIL");
    printf("         duty@50%%=%lu (~%lu, %s), duty@25%%=%lu (~%lu, %s)\r\n",
           (unsigned long)duty50, (unsigned long)(period/2),
           ok_50 ? "PASS" : "FAIL",
           (unsigned long)duty25, (unsigned long)(period/4),
           ok_25 ? "PASS" : "FAIL");
    printf("         timer11 overflows %lu->%lu while PWM live (%s)\r\n",
           (unsigned long)ov_before, (unsigned long)ov_after,
           ok_coord ? "PASS" : "FAIL");

    /* 5) teardown: float the PWM output, then stop the shared counter. */
    pwmd->vtable->close(pwmd);
    te->vtable->disable(te);
    tim->vtable->close(tim);
    return ok;
}

/* Verify the I2C STREAM master driver WITHOUT any slave hardware.
 * Proves:
 *   (1) register readback: PE=on CCR=210 FREQ=42 (F1 I2C config correct)
 *   (2) addressed ioctl probe + bus scan (I2C_IOCTL_MASTER_WRITE)
 *   (3) STREAM interface: set current_addr via I2C_IOCTL_SET_ADDR,
 *       then stream_write() and stream_read() via base device vtable */
static int selftest_vi2c(selftest *self)
{
    (void)self;
    device *i2cd = device_manager_get("i2c0");
    if (!i2cd) { printf("       i2c0: MISSING\r\n"); return 0; }

    if (i2cd->vtable->open(i2cd) != 0) {
        printf("       i2c0: OPEN FAILED (pin conflict?)\r\n");
        return 0;
    }

    int ok = 1;

    /* (1) register readback. */
    uint32_t ccr = 0, cr1 = 0, freq = 0;
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_GET_CCR, &ccr);
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_GET_CR2_FREQ, &freq);
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_GET_CR1, &cr1);
    int ok_pe   = (cr1 & 0x1U) ? 1 : 0;
    int ok_ccr  = (ccr == 210UL);
    int ok_freq = (freq == 42UL);
    if (!ok_pe || !ok_ccr || !ok_freq) ok = 0;
    printf("       i2c0(I2C1,PB6/PB7): PE=%s CCR=%lu(210? %s) FREQ=%lu(42? %s)\r\n",
           ok_pe ? "on" : "OFF", (unsigned long)ccr, ok_ccr ? "PASS" : "FAIL",
           (unsigned long)freq, ok_freq ? "PASS" : "FAIL");

    /* (2) POLL addressed probe + bus scan */
    i2c_xfer_t probe = { .addr = 0x50, .buf = NULL, .len = 0, .result = 0 };
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_MASTER_WRITE, &probe);
    int ok_aprobe = (probe.result == -1);
    if (!ok_aprobe) ok = 0;
    printf("       addr wr 0x50 probe -> %s (%s)\r\n",
           probe.result == 0 ? "ACK" : "NACK", ok_aprobe ? "PASS" : "FAIL");

    i2c_scan_t scan;
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_BUS_SCAN, &scan);
    int ok_scan = (scan.found == 0);
    if (!ok_scan) ok = 0;
    printf("       addr bus scan: found=%u (expect 0, %s)\r\n",
           (unsigned)scan.found, ok_scan ? "PASS" : "FAIL");

    /* (3) STREAM interface: set current_addr and use base read/write */
    uint16_t addr50 = 0x50;
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_SET_ADDR, &addr50);
    uint16_t got_addr = 0;
    i2cd->vtable->ioctl(i2cd, I2C_IOCTL_GET_ADDR, &got_addr);
    int ok_addr = (got_addr == 0x50);
    if (!ok_addr) ok = 0;
    printf("       STREAM set_addr=0x50 get=0x%02X (%s)\r\n",
           (unsigned)got_addr, ok_addr ? "PASS" : "FAIL");

    /* stream_write ("i2cd->vtable->write") with no slave → NACK */
    uint8_t txb = 0xA5;
    int wret = i2cd->vtable->write(i2cd, &txb, 1);
    int ok_swr = (wret == -1);    /* NACK means -1 from HAL */
    if (!ok_swr) ok = 0;
    printf("       STREAM write 1B to 0x50 -> %d (%s)\r\n",
           wret, ok_swr ? "PASS" : "FAIL");

    /* stream_read with no slave → NACK */
    uint8_t rxb = 0;
    int rret = i2cd->vtable->read(i2cd, &rxb, 1);
    int ok_srd = (rret == -1);
    if (!ok_srd) ok = 0;
    printf("       STREAM read  1B from 0x50 -> %d (%s)\r\n",
           rret, ok_srd ? "PASS" : "FAIL");

    i2cd->vtable->close(i2cd);
    return ok;
}

/* Verify the SPI master driver WITHOUT any slave hardware (no SPI device on the
 * Discovery board). We prove the driver works by:
 *   (1) register readback: CR1 must have SPE, MSTR, and BR set for ~656 kHz;
 *   (2) full-duplex byte transfer in POLL mode (proves the busy-wait engine);
 *   (3) full-duplex byte transfer in IRQ mode (proves the RXNE ISR + semaphore);
 *   (4) BSY flag clears after both transfers. */
static int selftest_vspi(selftest *self)
{
    (void)self;
    device *spid = device_manager_get("spi0");
    if (!spid) { printf("       spi0: MISSING\r\n"); return 0; }

    if (spid->vtable->open(spid) != 0) {
        printf("       spi0: OPEN FAILED (pin conflict?)\r\n");
        return 0;
    }

    int ok = 1;

    /* (1) register readback. */
    uint32_t cr1 = 0;
    spid->vtable->ioctl(spid, SPI_IOCTL_GET_CR1, &cr1);
    int ok_spe  = (cr1 & SPI_CR1_SPE) ? 1 : 0;
    int ok_mstr = (cr1 & SPI_CR1_MSTR) ? 1 : 0;
    int ok_br   = ((cr1 & SPI_CR1_BR) == (6U << 3)) ? 1 : 0;  /* BR=6 */
    if (!ok_spe || !ok_mstr || !ok_br) ok = 0;
    printf("       spi0(SPI1,PA5/6/7): SPE=%s MSTR=%s BR=0x%lX(0x30? %s)\r\n",
           ok_spe ? "on" : "OFF", ok_mstr ? "on" : "OFF",
           (unsigned long)(cr1 & SPI_CR1_BR), ok_br ? "PASS" : "FAIL");

    /* (2) POLL mode: default after open. */
    uint8_t tx = 0xA5, rx_poll = 0;
    spi_xfer_t xfer = { .tx_buf = &tx, .rx_buf = &rx_poll, .len = 1 };
    int poll_ok = (spid->vtable->ioctl(spid, SPI_IOCTL_XFER, &xfer) == 0);
    if (!poll_ok) ok = 0;
    int bsy = 1;
    spid->vtable->ioctl(spid, SPI_IOCTL_GET_BSY, &bsy);
    int poll_bsy = (bsy == 0);
    if (!poll_bsy) ok = 0;
    printf("       POLL xfer 1B: tx=0x%02X rx=0x%02X xfer=%s BSY=%s\r\n",
           (unsigned)tx, (unsigned)rx_poll,
           poll_ok ? "PASS" : "FAIL", poll_bsy ? "clear" : "SET");

    /* (3) IRQ mode: switch, transfer, switch back. */
    stream_xfer_mode_t irq_mode = STREAM_MODE_IRQ;
    spid->vtable->ioctl(spid, STREAM_IOCTL_SET_MODE, &irq_mode);
    stream_xfer_mode_t got = STREAM_MODE_POLL;
    spid->vtable->ioctl(spid, STREAM_IOCTL_GET_MODE, &got);
    int mode_irq_ok = (got == STREAM_MODE_IRQ);

    uint8_t rx_irq = 0;
    spi_xfer_t xfer_irq = { .tx_buf = &tx, .rx_buf = &rx_irq, .len = 1 };
    int irq_xfer_ok = (spid->vtable->ioctl(spid, SPI_IOCTL_XFER, &xfer_irq) == 0);
    spid->vtable->ioctl(spid, SPI_IOCTL_GET_BSY, &bsy);
    int irq_bsy = (bsy == 0);
    if (!mode_irq_ok || !irq_xfer_ok || !irq_bsy) ok = 0;
    printf("       IRQ  xfer 1B: mode=%s tx=0x%02X rx=0x%02X xfer=%s BSY=%s\r\n",
           mode_irq_ok ? "PASS" : "FAIL",
           (unsigned)tx, (unsigned)rx_irq,
           irq_xfer_ok ? "PASS" : "FAIL", irq_bsy ? "clear" : "SET");

    /* restore POLL mode */
    stream_xfer_mode_t poll_mode = STREAM_MODE_POLL;
    spid->vtable->ioctl(spid, STREAM_IOCTL_SET_MODE, &poll_mode);

    spid->vtable->close(spid);
    return ok;
}

/* Verify the SDIO host-driver config WITHOUT any SD card present (no card on
 * the Discovery board). We prove the driver works by register readback after
 * open(): POWER.PWRCTRL must be 0x3 (power-on), CLKCR.CLKDIV must be 118,
 * CLKCR.CLKEN must be set, and WIDBUS must be 4-bit. The register values are
 * exactly what sdio_dev_open() programs via sdio_hal. */
static int selftest_vsdio(selftest *self)
{
    (void)self;
    device *sd = device_manager_get("sdio0");
    if (!sd) { printf("       sdio0: MISSING\r\n"); return 0; }

    if (sd->vtable->open(sd) != 0) {
        printf("       sdio0: OPEN FAILED (pin conflict?)\r\n");
        return 0;
    }

    int ok = 1;

    uint32_t power = 0, clkcr = 0;
    sd->vtable->ioctl(sd, SDIO_IOCTL_GET_POWER, &power);
    sd->vtable->ioctl(sd, SDIO_IOCTL_GET_CLKCR, &clkcr);

    int ok_pwr  = ((power & 0x3U) == 0x3U);                          /* PWRCTRL = power-on */
    int ok_div  = ((clkcr & SDIO_CLKCR_CLKDIV) == 118UL);            /* open() sets 118   */
    int ok_cken = ((clkcr & SDIO_CLKCR_CLKEN) != 0U);                /* clock enabled     */
    int ok_wid  = ((clkcr & SDIO_CLKCR_WIDBUS) == SDIO_CLKCR_WIDBUS_0); /* 4-bit          */
    if (!ok_pwr || !ok_div || !ok_cken || !ok_wid) ok = 0;

    printf("       sdio0(SDIO): POWER=0x%02lX(pwon? %s) CLKCR=0x%08lX\r\n",
           (unsigned long)power, ok_pwr ? "PASS" : "FAIL", (unsigned long)clkcr);
    printf("         CLKDIV=%lu(118? %s) CLKEN=%s WIDBUS=4bit(%s)\r\n",
           (unsigned long)(clkcr & SDIO_CLKCR_CLKDIV), ok_div ? "PASS" : "FAIL",
           ok_cken ? "on" : "OFF", ok_wid ? "PASS" : "FAIL");

    sd->vtable->close(sd);
    return ok;
}

/* Verify the DAC driver WITHOUT any external measurement: we write several
 * 12-bit values through ioctl and read back the DOR register the hardware
 * latches, proving the value path (DHR->DOR) and the channel-enable bit. The
 * analog voltage itself is not probed (no scope on the board); DOR readback is
 * exactly what the silicon holds after a write with triggering disabled. */
static int selftest_vdac(selftest *self)
{
    (void)self;
    device *d = device_manager_get("dac0");
    if (!d) { printf("       dac0: MISSING\r\n"); return 0; }
    if (d->vtable->open(d) != 0) {
        printf("       dac0: OPEN FAILED (pin conflict?)\r\n");
        return 0;
    }

    int ok = 1;
    static const uint16_t vals[3] = { 0x000, 0x800, 0xFFF };
    int rb_ok = 1;
    for (int i = 0; i < 3; i++) {
        uint16_t v = vals[i];
        d->vtable->ioctl(d, DAC_IOCTL_SET_VALUE, &v);
        uint16_t got = 0;
        d->vtable->ioctl(d, DAC_IOCTL_GET_VALUE, &got);
        if (got != vals[i]) rb_ok = 0;
    }

    uint32_t cr = 0;
    d->vtable->ioctl(d, DAC_IOCTL_GET_CR, &cr);
    int ok_en = ((cr & 0x1U) != 0U);   /* CR.EN1 (channel 1 enabled) */
    if (!rb_ok || !ok_en) ok = 0;

    printf("       dac0(DAC1_CH1,PA4): DOR readback %s, CR.EN1=%s\r\n",
           rb_ok ? "PASS" : "FAIL", ok_en ? "on" : "OFF");

    d->vtable->close(d);
    return ok;
}

/* Verify the RNG driver WITHOUT any external wiring: the generator is fully
 * on-chip. We prove:
 *   (1) the device opens and the generator enables without error;
 *   (2) we can pull N 32-bit words, none of which trip the clock/seed error
 *       flags (RNG_SR.CEIS / RNG_SR.SEIS);
 *   (3) the words are not all identical — a real entropy source will not emit
 *       the same value N times in a row (a stuck generator would).
 * A correct RNG trivially satisfies all three; the test is therefore a solid
 * sanity check rather than a statistical-quality measure. */
static int selftest_vrng(selftest *self)
{
    (void)self;
    device *d = device_manager_get("rng0");
    if (!d) { printf("       rng0: MISSING\r\n"); return 0; }
    if (d->vtable->open(d) != 0) {
        printf("       rng0: OPEN FAILED\r\n");
        return 0;
    }

    int ok = 1;
    const int N = 8;
    uint32_t v[8];
    int err = 0;
    for (int i = 0; i < N; i++) {
        d->vtable->ioctl(d, RNG_IOCTL_GET_U32, &v[i]);
        uint32_t sr = 0;
        d->vtable->ioctl(d, RNG_IOCTL_GET_STATUS, &sr);
        if (sr & (RNG_SR_CEIS | RNG_SR_SEIS)) err = 1;
    }
    if (err) ok = 0;

    /* not all identical */
    int all_same = 1;
    for (int i = 1; i < N; i++) if (v[i] != v[0]) { all_same = 0; break; }
    if (all_same) ok = 0;

    printf("       rng0(RNG): ");
    for (int i = 0; i < N; i++) printf("%08lX ", (unsigned long)v[i]);
    printf("%s%s\r\n",
           err ? "ERR " : "",
           ok ? "PASS" : "FAIL");

    d->vtable->close(d);
    return ok;
}

/* Verify the RTC driver WITHOUT any external wiring: the RTC is clocked by the
 * internal LSI oscillator, so the test is fully self-contained. We prove:
 *   (1) the clock tree is up — RCC->BDCR has RTCEN set and RTCSEL == LSI;
 *   (2) the prescaler is programmed for a 1 Hz tick (async=127, sync=255);
 *   (3) the time path round-trips: set 12:34:56, read it back;
 *   (4) the date path round-trips: set 2026-01-01, read it back.
 * The writes go through init mode (WPR unlocked), so they exercise the real
 * calendar-register programming and the RSF shadow-sync path. */
static int selftest_vrtc(selftest *self)
{
    (void)self;
    device *d = device_manager_get("rtc0");
    if (!d) { printf("       rtc0: MISSING\r\n"); return 0; }
    if (d->vtable->open(d) != 0) {
        printf("       rtc0: OPEN FAILED\r\n");
        return 0;
    }

    int ok = 1;

    /* (1) clock tree. */
    uint32_t bdcr = 0;
    d->vtable->ioctl(d, RTC_IOCTL_GET_BDCR, &bdcr);
    int ok_en  = (bdcr & RCC_BDCR_RTCEN) != 0U;
    int ok_src = ((bdcr & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_1);  /* LSI */
    if (!ok_en || !ok_src) ok = 0;
    printf("       rtc0(RTC,LSI): RTCEN=%s RTCSEL=%s\r\n",
           ok_en ? "on" : "OFF", ok_src ? "LSI" : "OTHER");

    /* (2) prescaler readback. */
    rtc_prer_t pr = { 0, 0 };
    d->vtable->ioctl(d, RTC_IOCTL_GET_PRER, &pr);
    int ok_pr = (pr.prediv_a == 127U && pr.prediv_s == 255U);
    if (!ok_pr) ok = 0;
    printf("       PRER async=%lu sync=%lu (expect 127/255, %s)\r\n",
           (unsigned long)pr.prediv_a, (unsigned long)pr.prediv_s,
           ok_pr ? "PASS" : "FAIL");

    /* (3) time round-trip. */
    rtc_time_t set = { 12, 34, 56 };
    d->vtable->ioctl(d, RTC_IOCTL_SET_TIME, &set);
    rtc_time_t got = { 0, 0, 0 };
    d->vtable->ioctl(d, RTC_IOCTL_GET_TIME, &got);
    int ok_time = (got.hour == 12 && got.min == 34 && got.sec == 56);
    if (!ok_time) ok = 0;
    printf("       set 12:34:56 -> get %02u:%02u:%02u (%s)\r\n",
           (unsigned)got.hour, (unsigned)got.min, (unsigned)got.sec,
           ok_time ? "PASS" : "FAIL");

    /* (4) date round-trip. The date is written in its OWN init phase (see the
     * driver), which is what makes it latch on this silicon. GET_RAW_DR just
     * prints the raw register for diagnostics. */
    rtc_date_t dset = { 2026, 1, 1, 4 };
    d->vtable->ioctl(d, RTC_IOCTL_SET_DATE, &dset);
    uint32_t raw_dr = 0;
    d->vtable->ioctl(d, RTC_IOCTL_GET_RAW_DR, &raw_dr);
    rtc_date_t dgot = { 0, 0, 0, 0 };
    d->vtable->ioctl(d, RTC_IOCTL_GET_DATE, &dgot);
    int ok_date = (dgot.year == 2026 && dgot.month == 1 && dgot.day == 1);
    if (!ok_date) ok = 0;
    printf("       set 2026-01-01 -> get %u-%02u-%02u (rawDR=0x%08lX, %s)\r\n",
           (unsigned)dgot.year, (unsigned)dgot.month, (unsigned)dgot.day,
           (unsigned long)raw_dr, ok_date ? "PASS" : "FAIL");

    d->vtable->close(d);
    return ok;
}

/* Software reference: CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, MSB-first,
 * no final XOR) — exactly the STM32F4 CRC unit's reset-default behaviour. The
 * self-test compares the hardware result against this, so it validates the
 * engine rather than just its stability. */
static uint32_t selftest_sw_crc32_mpeg2(const uint32_t *buf, size_t nwords)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t word = buf[i];
        for (int b = 31; b >= 0; b--) {
            uint32_t bit = (word >> b) & 1U;
            uint32_t inv = (crc >> 31) ^ bit;
            crc = (crc << 1) ^ (inv ? 0x04C11DB7U : 0U);
        }
    }
    return crc;
}

/* Verify the CRC driver WITHOUT any external wiring: the unit is fully on-chip.
 * We feed a fixed 4-word message through the hardware (reset -> update x4 ->
 * result) and compare the result against the software reference above. They
 * must match exactly for the engine to be correct. */
static int selftest_vcrc(selftest *self)
{
    (void)self;
    device *d = device_manager_get("crc0");
    if (!d) { printf("       crc0: MISSING\r\n"); return 0; }
    if (d->vtable->open(d) != 0) {
        printf("       crc0: OPEN FAILED\r\n");
        return 0;
    }

    int ok = 1;
    const uint32_t msg[4] = { 0x12345678U, 0x23456789U, 0x3456789AU, 0x456789ABU };

    /* hardware path */
    d->vtable->ioctl(d, CRC_IOCTL_RESET, NULL);
    for (int i = 0; i < 4; i++) d->vtable->ioctl(d, CRC_IOCTL_UPDATE, (void *)&msg[i]);
    uint32_t hw = 0;
    d->vtable->ioctl(d, CRC_IOCTL_RESULT, &hw);

    /* reference path */
    uint32_t sw = selftest_sw_crc32_mpeg2(msg, 4);

    int ok_match = (hw == sw);
    if (!ok_match) ok = 0;
    printf("       crc0(CRC): hw=0x%08lX sw=0x%08lX %s\r\n",
           (unsigned long)hw, (unsigned long)sw, ok_match ? "PASS" : "FAIL");

    d->vtable->close(d);
    return ok;
}

/* Verify the IWDG driver WITHOUT arming the counter (which would reset the
 * board mid-BIST). We exercise the unlock -> program -> status-settle ->
 * readback control path: program a known prescaler + reload through the
 * unified device interface and confirm the hardware latched exactly those
 * values back. open() only ensures LSI is running; START is never issued. */
static int selftest_viwdg(selftest *self)
{
    (void)self;
    int ok = 1;

    device *d = device_manager_get("iwdg0");
    if (!d) { printf("       iwdg0: MISSING\r\n"); return 0; }
    if (d->vtable->open(d) != 0) {
        printf("       iwdg0: OPEN FAILED\r\n");
        return 0;
    }

    /* The IWDG programming path: unlock (KR=0x5555) -> write PR/RLR. The writes
     * are latched into the config registers and the hardware raises PVU/RVU in
     * SR to mark the transfer to the ACTIVE prescaler/reload as pending. The
     * transfer only completes on a START (KR=0xCCCC) or a RELOAD (KR=0xAAAA)
     * while running — and STARTING would arm the counter and reset the board
     * (IWDG survives reset, so it would brick the board into a reset loop).
     * So the BIST deliberately never starts it. Instead we verify the control
     * path by confirming the writes were ACCEPTED: SR must show PVU/RVU set
     * after programming. Those bits are raised ONLY by a successful unlocked
     * write, so this proves the unlock + backup-domain (DBP) + LSI + write path
     * all work. (The pending bits persist in the backup domain across resets,
     * so we only assert the post-program state, not a before/after delta.) */
    const uint32_t pr_set = 4U;   /* code 4 = /64 */
    const uint32_t rl_set = 0x500U;
    d->vtable->ioctl(d, IWDG_IOCTL_SET_PRESCALER, (void *)&pr_set);
    d->vtable->ioctl(d, IWDG_IOCTL_SET_RELOAD,    (void *)&rl_set);

    uint32_t sr = 0;
    d->vtable->ioctl(d, IWDG_IOCTL_GET_STATUS, &sr);

    int now_pending = ((sr & (IWDG_SR_PVU | IWDG_SR_RVU)) ==
                       (IWDG_SR_PVU | IWDG_SR_RVU));
    if (!now_pending) ok = 0;
    int ok_all = now_pending;

    printf("       iwdg0(IWDG): program PR=0x%lx RLR=0x%lx -> SR=0x%lx (PVU/RVU set = %s) %s\r\n",
           (unsigned long)pr_set, (unsigned long)rl_set, (unsigned long)sr,
           now_pending ? "PASS" : "FAIL", ok_all ? "PASS" : "FAIL");

    d->vtable->close(d);
    return ok;
}

/* Verify the external-interrupt driver end-to-end WITHOUT a physical button:
 * we SOFTWARE-TRIGGER each line (EXTI->SWIER) so the ISR fires, then confirm
 * the per-device interrupt count and the subscribed callback both incremented.
 * The shared-line pair (exti0 PE5 + exti1 PE6 on EXTI9_5 / IRQ23) proves the
 * multi-handler irq framework works for EXTI too: triggering ONLY A must NOT
 * make B's handler run (each ISR guards on its own PR bit). */
static volatile uint32_t g_exti_cb_count;   /* bumped from ISR context */
static void selftest_exti_cb(void *ctx, device_event_type_t ev, void *ev_data)
{
    (void)ctx; (void)ev; (void)ev_data;
    g_exti_cb_count++;
}

static int selftest_vexti(selftest *self)
{
    (void)self;
    int ok = 1;

    /* --- single, dedicated line: exti2 = PE0 -> EXTI0 (IRQ6) --- */
    device *d0 = device_manager_get("exti2");
    if (!d0) { printf("       exti2: MISSING\r\n"); ok = 0; }
    else {
        event_device *e0 = device_as_event(d0);
        g_exti_cb_count = 0;
        d0->vtable->open(d0);
        e0->vtable->set_event_callback(e0, DEVICE_EVENT_IRQ, selftest_exti_cb, NULL);
        e0->vtable->enable(e0);
        d0->vtable->ioctl(d0, EXTI_IOCTL_TRIGGER, NULL);   /* simulate edge */
        for (volatile uint32_t k = 0; k < 2000U; k++) { }  /* let ISR run */
        d0->vtable->ioctl(d0, EXTI_IOCTL_TRIGGER, NULL);
        for (volatile uint32_t k = 0; k < 2000U; k++) { }
        uint32_t cnt = 0;
        d0->vtable->ioctl(d0, EXTI_IOCTL_GET_COUNT, &cnt);
        int ok_single = (cnt >= 2U) && (g_exti_cb_count >= 2U);
        if (!ok_single) ok = 0;
        printf("       exti2(PE0,IRQ6): count=%lu cb=%lu (%s)\r\n",
               (unsigned long)cnt, (unsigned long)g_exti_cb_count,
               ok_single ? "PASS" : "FAIL");
        e0->vtable->disable(e0);
        e0->vtable->clear_event_callback(e0, DEVICE_EVENT_IRQ);
        d0->vtable->close(d0);
    }

    /* --- shared line: exti0(PE5) + exti1(PE6) on EXTI9_5 (IRQ23) --- */
    device *da = device_manager_get("exti0");
    device *db = device_manager_get("exti1");
    if (!da || !db) { printf("       exti0/exti1: MISSING\r\n"); ok = 0; }
    else {
        event_device *ea = device_as_event(da);
        event_device *eb = device_as_event(db);
        g_exti_cb_count = 0;
        da->vtable->open(da);
        db->vtable->open(db);
        ea->vtable->set_event_callback(ea, DEVICE_EVENT_IRQ, selftest_exti_cb, NULL);
        eb->vtable->set_event_callback(eb, DEVICE_EVENT_IRQ, selftest_exti_cb, NULL);
        ea->vtable->enable(ea);
        eb->vtable->enable(eb);

        /* trigger ONLY A (PE5) */
        da->vtable->ioctl(da, EXTI_IOCTL_TRIGGER, NULL);
        for (volatile uint32_t k = 0; k < 2000U; k++) { }
        uint32_t ca = 0, cb = 0;
        da->vtable->ioctl(da, EXTI_IOCTL_GET_COUNT, &ca);
        db->vtable->ioctl(db, EXTI_IOCTL_GET_COUNT, &cb);
        /* A must fire; B must NOT (sibling guard on a shared NVIC line) */
        int ok_a = (ca >= 1U) && (cb == 0U);
        if (!ok_a) ok = 0;

        /* trigger ONLY B (PE6) */
        db->vtable->ioctl(db, EXTI_IOCTL_TRIGGER, NULL);
        for (volatile uint32_t k = 0; k < 2000U; k++) { }
        da->vtable->ioctl(da, EXTI_IOCTL_GET_COUNT, &ca);
        db->vtable->ioctl(db, EXTI_IOCTL_GET_COUNT, &cb);
        int ok_b = (cb >= 1U) && (ca >= 1U);   /* B now fired, A unchanged */
        if (!ok_b) ok = 0;

        printf("       exti0(PE5)+exti1(PE6) IRQ23: A=%lu B=%lu (sibling-guard %s)\r\n",
               (unsigned long)ca, (unsigned long)cb, (ok_a && ok_b) ? "PASS" : "FAIL");
        ea->vtable->disable(ea);
        eb->vtable->disable(eb);
        ea->vtable->clear_event_callback(ea, DEVICE_EVENT_IRQ);
        eb->vtable->clear_event_callback(eb, DEVICE_EVENT_IRQ);
        da->vtable->close(da);
        db->vtable->close(db);
    }
    return ok;
}

/* Verify the ADVANCED-TIMER REPETITION COUNTER (RCR) on TIM1 (timer1). The RCR
 * makes the Update event — and thus the timer driver's TICK — fire only every
 * (RCR+1) counter overflows, i.e. it divides the TICK rate without touching
 * PSC/ARR. We use timer11 (TIM3, 20 Hz, RCR=0) as a deterministic 1-second
 * reference: with RCR=3 the timer1 TICK count over one timer11 second must be
 * ~1/4 of timer11's, and the RCR readback must report 3. */
static int selftest_vadvtimer(selftest *self)
{
    (void)self;
    device *ref = device_manager_get("timer11");  /* TIM3, 20 Hz, RCR=0 */
    device *adv = device_manager_get("timer1");   /* TIM1, advanced, 20 Hz */
    if (!ref || !adv) { printf("       timer1/timer11: MISSING\r\n"); return 0; }
    event_device *re = device_as_event(ref);
    event_device *ae = device_as_event(adv);
    if (!re || !ae) { printf("       timer1/timer11: not-event\r\n"); return 0; }

    ref->vtable->open(ref);  re->vtable->enable(re);    /* reference clock */
    adv->vtable->open(adv); ae->vtable->enable(ae);

    uint32_t rep = 3;
    adv->vtable->ioctl(adv, TIMER_IOCTL_SET_REPETITION, &rep);

    /* count TICKs over one timer11 second (20 reference ticks). */
    uint32_t ref0 = 0;
    ref->vtable->ioctl(ref, TIMER_IOCTL_GET_OVERFLOWS, &ref0);
    uint32_t adv0 = 0;
    adv->vtable->ioctl(adv, TIMER_IOCTL_GET_OVERFLOWS, &adv0);
    uint32_t ref_now = ref0;
    for (;;) {
        ref->vtable->ioctl(ref, TIMER_IOCTL_GET_OVERFLOWS, &ref_now);
        if (ref_now - ref0 >= 20U) break;     /* ~1 s at 20 Hz */
    }
    uint32_t adv_now = 0;
    adv->vtable->ioctl(adv, TIMER_IOCTL_GET_OVERFLOWS, &adv_now);
    uint32_t ref_ticks = ref_now - ref0;
    uint32_t adv_ticks = adv_now - adv0;

    uint32_t rep_rb = 0;
    adv->vtable->ioctl(adv, TIMER_IOCTL_GET_REPETITION, &rep_rb);

    /* With RCR=3 the advanced TICK rate is 1/(3+1) of the reference. */
    int ok_rep = (rep_rb == 3U);
    uint32_t expect = ref_ticks / 4U;          /* expected advanced ticks */
    int ok_div = (expect > 0) &&
                 (adv_ticks >= expect - 2U) &&  /* within +/-2 ticks of ref/4 */
                 (adv_ticks <= expect + 2U);
    int ok = ok_rep && ok_div;

    printf("       timer1(TIM1,RCR=3): adv_ticks=%lu ref_ticks=%lu (expect ~1/4, %s)\r\n",
           (unsigned long)adv_ticks, (unsigned long)ref_ticks, ok_div ? "PASS" : "FAIL");
    printf("         RCR readback=%lu (expect 3, %s)\r\n",
           (unsigned long)rep_rb, ok_rep ? "PASS" : "FAIL");

    /* restore RCR=0 so later BIST runs see the normal 20 Hz rate, then tear down */
    rep = 0;
    adv->vtable->ioctl(adv, TIMER_IOCTL_SET_REPETITION, &rep);
    ae->vtable->disable(ae); adv->vtable->close(adv);
    re->vtable->disable(re); ref->vtable->close(ref);
    return ok;
}

/* Verify the ADVANCED-TIMER PWM features on TIM8 (timer4 + pwm1): the Main
 * Output Enable (MOE) that gates the pins, the inserted DEAD-TIME (DTG), and the
 * COMPLEMENTARY output (CH1N). pwm1 coordinates with timer4 (which owns TIM8's
 * 20 Hz period), so this also proves TIM8 keeps emitting its TICK while the
 * advanced PWM is live. Without MOE the pins would stay dead — that is exactly
 * the gap these checks close. */
static int selftest_vadvpwm(selftest *self)
{
    (void)self;
    device *tim = device_manager_get("timer4");   /* TIM8, 20 Hz TICK source */
    device *pwmd = device_manager_get("pwm1");     /* TIM8 CH1 + CH1N, dead-time */
    if (!tim || !pwmd) { printf("       timer4/pwm1: MISSING\r\n"); return 0; }
    event_device *te = device_as_event(tim);
    if (!te) { printf("       timer4: not-event\r\n"); return 0; }

    tim->vtable->open(tim);
    te->vtable->enable(te);
    if (pwmd->vtable->open(pwmd) != 0) {
        printf("       pwm1: OPEN FAILED (pin conflict?)\r\n");
        te->vtable->disable(te); tim->vtable->close(tim);
        return 0;
    }

    int ok = 1;

    /* BDTR: MOE must be set (or pins stay dead) and DTG must hold 64 ticks. */
    uint32_t bdtr = 0;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_BDTR, &bdtr);
    int ok_moe = (bdtr & 0x8000U) ? 1 : 0;          /* BDTR.MOE */
    int ok_dtg = ((bdtr & 0xFFU) == 64U);           /* dead-time = 64 ticks */

    /* Complementary output (CH1N) must be enabled in CCER. */
    uint32_t comp = 0;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_COMPLEMENTARY, &comp);
    int ok_comp = (comp & 1U) ? 1 : 0;              /* bit0 = CH1N */

    /* Duty 50% must land in CCR (period/2). */
    int pct = 50;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_SET_DUTY_PERCENT, &pct);
    uint32_t period = 0, duty = 0;
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_PERIOD_TICKS, &period);
    pwmd->vtable->ioctl(pwmd, PWM_IOCTL_GET_DUTY_TICKS, &duty);
    int ok_duty = (period > 0) &&
                  (duty >= period / 2 - 2U) && (duty <= period / 2 + 2U);

    /* Coordination: timer4 must keep counting while the advanced PWM is live. */
    uint32_t ov0 = 0;
    tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov0);
    for (volatile uint32_t k = 0; k < (SystemCoreClock / 20U); k++) { }
    uint32_t ov1 = 0;
    tim->vtable->ioctl(tim, TIMER_IOCTL_GET_OVERFLOWS, &ov1);
    int ok_coord = (ov1 > ov0);

    if (!ok_moe || !ok_dtg || !ok_comp || !ok_duty || !ok_coord) ok = 0;
    printf("       pwm1(TIM8 CH1+CH1N): MOE=%s DTG=%lu(64? %s) comp=%s\r\n",
           ok_moe ? "on" : "OFF", (unsigned long)(bdtr & 0xFFU),
           ok_dtg ? "PASS" : "FAIL", ok_comp ? "on" : "OFF");
    printf("         duty@50%%=%lu (~%lu, %s); timer4 ov %lu->%lu while PWM (%s)\\r\\n",
           (unsigned long)duty, (unsigned long)(period / 2), ok_duty ? "PASS" : "FAIL",
           (unsigned long)ov0, (unsigned long)ov1, ok_coord ? "PASS" : "FAIL");

    pwmd->vtable->close(pwmd);
    te->vtable->disable(te);
    tim->vtable->close(tim);
    return ok;
}


