#include "pwm.h"
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include "pinmux_hal.h"               /* pinmux_port_t (resolved port for claim) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "log/log.h"
#include "log/app_log.h"

/* virtual implementations dispatched through the unified device vtable */
static int pwm_dev_open(device *self);
static int pwm_dev_close(device *self);
static int pwm_dev_read(device *self, void *buf, size_t len);
static int pwm_dev_write(device *self, const void *buf, size_t len);
static int pwm_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (command/set/get) */
static int pwm_control_command(control_device *self, int cmd, void *arg);
static int pwm_control_set(control_device *self, int param, const void *val);
static int pwm_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable pwm_control_vtable = {
    .command = pwm_control_command,
    .set     = pwm_control_set,
    .get     = pwm_control_get,
};

static const struct deviceVtable pwm_dev_vtable = {
    .open  = pwm_dev_open,
    .close = pwm_dev_close,
    .read  = pwm_dev_read,
    .write = pwm_dev_write,
    .ioctl = pwm_dev_ioctl,
};

/* Set the duty from a 0..100 percentage of the current period. */
static void pwm_apply_percent(pwm *p, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* duty_ticks = round(percent/100 * period_ticks), clamped to period. */
    uint64_t d = ((uint64_t)percent * (uint64_t)p->period_ticks + 50ULL) / 100ULL;
    if (d > p->period_ticks) d = p->period_ticks;
    p->duty_ticks = (uint32_t)d;
    tim_hal_pwm_set_duty(p->hal, p->channel, p->duty_ticks);
}

device *pwm_create(const void *config)
{
    const pwm_config_t *c = (const pwm_config_t *)config;
    if (!c || c->channel < 1 || c->channel > 4 || !c->pin_signal)
        return NULL;

    pwm *p = (pwm *)malloc(sizeof(pwm));
    if (!p) return NULL;
    memset(p, 0, sizeof(pwm));

    /* Resolve the AF signal name (e.g. "TIM3_CH1_PA6") up front so we can claim
     * the pin through the pinmux later and know which pad we own. */
    pinmux_port_t port; uint8_t pin, af;
    if (!pinmux_hal_resolve(c->pin_signal, &port, &pin, &af)) {
        log_printf(app_log(), LOG_DEBUG, "pwm", "[pwm] %s: unknown signal \"%s\"\n", c->name, c->pin_signal);
        free(p);
        return NULL;
    }

    p->hal = tim_hal_create(c->peripheral);
    if (!p->hal) { free(p); return NULL; }

    p->parent.parent.vtable = &pwm_dev_vtable;
    p->parent.vtable        = &pwm_control_vtable;
    p->parent.parent.type   = DEVICE_TYPE_PWM;
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;   /* PWM IS-A control_device */
    p->parent.parent.name   = c->name;
    p->timer_clk_hz = c->timer_clk_hz;
    p->freq_hz      = c->freq_hz;
    p->channel      = c->channel;
    p->pin_signal   = c->pin_signal;
    p->port = port;
    p->pin  = pin;
    p->af   = af;
    p->coord = (c->freq_hz == 0) ? 1 : 0;
    p->period_ticks = 0;
    p->duty_ticks   = 0;

    /* Advanced-TIM options (TIM1/TIM8): resolve the complementary pin if given. */
    p->comp_port = 0; p->comp_pin = 0; p->comp_af = 0;
    p->deadtime_ticks = c->deadtime_ticks;
    p->complementary   = c->complementary;
    p->break_enable    = c->break_enable;
    p->break_polarity  = c->break_polarity;
    if (c->comp_pin_signal) {
        pinmux_port_t cp; uint8_t cpn, cpa;
        if (!pinmux_hal_resolve(c->comp_pin_signal, &cp, &cpn, &cpa)) {
            log_printf(app_log(), LOG_DEBUG, "pwm", "[pwm] %s: unknown complementary signal \"%s\"\n",
                   c->name, c->comp_pin_signal);
            free(p);
            return NULL;
        }
        p->comp_port = cp; p->comp_pin = cpn; p->comp_af = cpa;
    }

    return &p->parent.parent;
}

void pwm_destroy(pwm *self)
{
    if (!self) return;
    tim_hal_destroy(self->hal);
    free(self);
}

static int pwm_dev_open(device *self)
{
    pwm *p = (pwm *)self;

    /* Claim + program the output pin through the pinmux BEFORE touching TIM regs
     * (AF mode, very-high speed). A conflict makes request fail and we refuse. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, p->port, p->pin, p->af, p->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_ERROR, "pwm", "[pwm] %s: pin P%c%d AF%d CONFLICT — refused (rc=-2)\n",
                   p->parent.parent.name, 'A' + (int)p->port, (int)p->pin, (int)p->af);
            return -2;
        }
        pinmux_pin_cfg_t cfg = {
            .af = p->af, .mode = 2, .otype = 0, .speed = 3, .pupd = 0
        };
        pm->fun->config(pm, p->port, p->pin, &cfg);
        /* Complementary pin (advanced TIM only): same AF, also claimed. */
        if (p->comp_port) {
            if (pm->fun->request(pm, p->comp_port, p->comp_pin, p->comp_af,
                                 p->parent.parent.name) != 0) {
                log_printf(app_log(), LOG_DEBUG, "pwm", "[pwm] %s: complementary pin P%c%d CONFLICT — refused\n",
                       p->parent.parent.name, 'A' + (int)p->comp_port,
                       (int)p->comp_pin);
                return -2;
            }
            pinmux_pin_cfg_t ccfg = {
                .af = p->comp_af, .mode = 2, .otype = 0, .speed = 3, .pupd = 0
            };
            pm->fun->config(pm, p->comp_port, p->comp_pin, &ccfg);
        }
    }

    tim_hal_enable_clock(p->hal);

    if (p->coord) {
        /* Coordinate mode: reuse the period a sibling timer driver already set.
         * If the timer hasn't opened yet (ARR==0), fall back to a 1 kHz period
         * so we still produce output; the timer driver will reprogram ARR later
         * if it opens afterwards (rare — board orders timer before pwm). */
        uint32_t existing = tim_hal_pwm_period_ticks(p->hal);
        if (existing == 0)
            p->period_ticks = tim_hal_pwm_set_period(p->hal, p->timer_clk_hz, 1000);
        else
            p->period_ticks = existing;
    } else {
        /* Independent mode: this driver owns the period + counter. */
        p->period_ticks = tim_hal_pwm_set_period(p->hal, p->timer_clk_hz, p->freq_hz);
        tim_hal_start(p->hal);     /* begin counting (timer driver owns it in coord) */
    }
    if (p->period_ticks == 0) {
        log_printf(app_log(), LOG_ERROR, "pwm", "[pwm] %s: period_ticks==0 — refused (rc=-1)\n",
               p->parent.parent.name);
        return -1;
    }

    /* Configure the channel as PWM (mode 1, active-high) and start at 0% duty. */
    tim_hal_pwm_config_channel(p->hal, p->channel, 1, 0);
    pwm_apply_percent(p, 0);
    tim_hal_pwm_channel_enable(p->hal, p->channel, 1);

    /* Advanced-TIM (TIM1/TIM8) extras: dead-time, complementary outputs, break,
     * and the Main Output Enable. On a GP TIM these are no-ops, so a PWM device
     * on TIM2..TIM5 works unchanged. MOE MUST be set or the pins stay inactive. */
    if (tim_hal_is_advanced(p->hal)) {
        if (p->deadtime_ticks)
            tim_hal_pwm_set_deadtime(p->hal,
                                     tim_hal_pwm_encode_deadtime(p->deadtime_ticks));
        for (int ch = 1; ch <= 4; ch++)
            if (p->complementary & (1U << (ch - 1)))
                tim_hal_pwm_config_complementary(p->hal, ch, 0);
        if (p->break_enable)
            tim_hal_pwm_set_break(p->hal, 1, p->break_polarity);
        tim_hal_pwm_main_output_enable(p->hal, 1);   /* BDTR.MOE = 1 */
    }
    return 0;
}

static int pwm_dev_close(device *self)
{
    pwm *p = (pwm *)self;
    tim_hal_pwm_channel_enable(p->hal, p->channel, 0);   /* float the output */
    /* In coordinate mode the counter is owned by the timer driver, so we do NOT
     * stop it here. In independent mode we stop counting. */
    if (!p->coord)
        tim_hal_stop(p->hal);
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) pm->fun->release_owner(pm, p->parent.parent.name);
    return 0;
}

static int pwm_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int pwm_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

static int pwm_control_command(control_device *self, int cmd, void *arg)
{
    pwm *p = (pwm *)self;
    switch (cmd) {
    case PWM_IOCTL_SET_DUTY_PERCENT:
        if (!arg) return -1;
        pwm_apply_percent(p, *(const int *)arg);
        return 0;
    case PWM_IOCTL_SET_DUTY_TICKS:
        if (!arg) return -1;
        p->duty_ticks = *(const uint32_t *)arg;
        if (p->duty_ticks > p->period_ticks) p->duty_ticks = p->period_ticks;
        tim_hal_pwm_set_duty(p->hal, p->channel, p->duty_ticks);
        return 0;
    case PWM_IOCTL_SET_FREQ:
        if (!arg || p->coord) return -1;   /* only in independent mode */
        p->period_ticks = tim_hal_pwm_set_period(p->hal, p->timer_clk_hz,
                                                 *(const uint32_t *)arg);
        return (p->period_ticks != 0) ? 0 : -1;
    case PWM_IOCTL_GET_PERIOD_TICKS:
        if (arg) *(uint32_t *)arg = p->period_ticks;
        return 0;
    case PWM_IOCTL_GET_DUTY_TICKS:
        if (arg) *(uint32_t *)arg = tim_hal_pwm_get_duty(p->hal, p->channel);
        return 0;
    case PWM_IOCTL_ENABLE_CHANNEL:
        tim_hal_pwm_channel_enable(p->hal, p->channel, 1);
        return 0;
    case PWM_IOCTL_DISABLE_CHANNEL:
        tim_hal_pwm_channel_enable(p->hal, p->channel, 0);
        return 0;
    case PWM_IOCTL_GET_BDTR:
        if (arg) *(uint32_t *)arg = tim_hal_pwm_get_bdtr(p->hal);
        return 0;
    case PWM_IOCTL_GET_COMPLEMENTARY:
        if (arg) {
            uint32_t mask = 0;
            for (int ch = 1; ch <= 4; ch++)
                if (tim_hal_pwm_complementary_enabled(p->hal, ch))
                    mask |= (1U << (ch - 1));
            *(uint32_t *)arg = mask;
        }
        return 0;
    default:
        return -1;
    }
}

static int pwm_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int pwm_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int pwm_dev_ioctl(device *self, int cmd, void *arg)
    { return pwm_control_command((control_device *)self, cmd, arg); }
