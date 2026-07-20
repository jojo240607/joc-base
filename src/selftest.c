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
