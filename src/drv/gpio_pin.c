#include "gpio_pin.h"
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */
#include "log/log.h"
#include "log/app_log.h"

/* virtual implementations dispatched through the unified device vtable */
static int gpio_dev_open(device *self);
static int gpio_dev_close(device *self);
static int gpio_dev_read(device *self, void *buf, size_t len);
static int gpio_dev_write(device *self, const void *buf, size_t len);
static int gpio_dev_ioctl(device *self, int cmd, void *arg);

/* subclass vtable (defined below; forward-declared so gpio_pin_init can ref it) */
static const struct control_deviceVtable gpio_control_vtable;

/* public methods — `static`, reachable ONLY through self->fun-> */
static void gpio_pin_set(gpio_pin *self);
static void gpio_pin_reset(gpio_pin *self);
static void gpio_pin_toggle(gpio_pin *self);
static uint8_t gpio_pin_read(gpio_pin *self);

const struct gpio_pinFun gpio_pin_fun = {
    .destroy = gpio_pin_destroy,
    .init    = gpio_pin_init,
    .deinit  = gpio_pin_deinit,
    .set     = gpio_pin_set,
    .reset   = gpio_pin_reset,
    .toggle  = gpio_pin_toggle,
    .read    = gpio_pin_read,
};

/* one shared vtable for the whole GPIO-pin class — assigned by gpio_pin_init() */
static const struct deviceVtable gpio_dev_vtable = {
    .open  = gpio_dev_open,
    .close = gpio_dev_close,
    .read  = gpio_dev_read,
    .write = gpio_dev_write,
    .ioctl = gpio_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *gpio_pin_create(const void *config)
{
    const gpio_config_t *c = (const gpio_config_t *)config;
    gpio_pin *self = (gpio_pin *)malloc(sizeof(gpio_pin));
    if (!self) return NULL;
    memset(self, 0, sizeof(gpio_pin));

    /* Resolve the board's signal name (e.g. "GPIOD_12") to the exact
     * (port, pin, af) up front so we can build the opaque HAL handle AND claim
     * the pin through the pinmux later. A plain GPIO pin uses af = 0. */
    pinmux_port_t port; uint8_t pin, af;
    if (!pinmux_hal_resolve(c->signal, &port, &pin, &af)) {
        log_printf(app_log(), LOG_DEBUG, "gpio", "[gpio] %s: unknown signal \"%s\"\n", c->name, c->signal);
        free(self);
        return NULL;
    }
    void *base = pinmux_hal_port_base(port);
    self->hal = gpio_hal_create(base, pin, c->mode);
    if (!self->hal) { free(self); return NULL; }   /* #9: HAL alloc failure */
    self->mode = c->mode;                    /* cache for pinmux config at open() */
    self->signal = c->signal;                /* cache name for diagnostics */
    self->port = port;                       /* cache resolved pad for claim */
    self->pin = pin;
    self->af = af;
    self->parent.parent.type = DEVICE_TYPE_GPIO;    /* driver sets its own class */
    self->parent.parent.name = c->name;             /* driver sets its own name */
    gpio_pin_init(self);
    return (device *)self;
}

void gpio_pin_destroy(gpio_pin *self)
{
    if (!self) return;
    gpio_pin_deinit(self);
    gpio_hal_destroy(self->hal);   /* mirror create: free the HAL handle */
    free(self);
}

void gpio_pin_init(gpio_pin *self)
{
    if (!self) return;
    self->parent.parent.vtable = &gpio_dev_vtable;       /* base device vtable */
    self->parent.vtable        = &gpio_control_vtable;   /* control-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_GPIO;
    self->parent.parent.class  = DEVICE_CLASS_CONTROL;
    self->fun = &gpio_pin_fun;
    /* hardware bring-up is deferred to open() (see gpio_dev_open) */
}

void gpio_pin_deinit(gpio_pin *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
}

static void gpio_pin_set(gpio_pin *self)   { if (self) gpio_hal_set(self->hal); }
static void gpio_pin_reset(gpio_pin *self) { if (self) gpio_hal_reset(self->hal); }
static void gpio_pin_toggle(gpio_pin *self){ if (self) gpio_hal_toggle(self->hal); }
static uint8_t gpio_pin_read(gpio_pin *self){ return self ? gpio_hal_read(self->hal) : 0U; }

/* --- unified device-interface virtual implementations --- */

static int gpio_dev_open(device *self)
{
    gpio_pin *g = (gpio_pin *)self;

    /* Claim + program the pin through the pinmux BEFORE configuring anything.
     * The pad (port, pin, af) was resolved from the board's signal name at
     * create() time, so there is no ambiguity. A conflict (pin already owned
     * elsewhere) makes request fail and we refuse to touch the hardware. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        if (pm->fun->request(pm, g->port, g->pin, g->af, g->parent.parent.name) != 0) {
            log_printf(app_log(), LOG_DEBUG, "gpio", "[gpio] %s: pin P%c%d CONFLICT — refused\n",
                   g->parent.parent.name, 'A' + g->port, (int)g->pin);
            return -2;                       /* conflict: do NOT configure */
        }
        pinmux_pin_cfg_t cfg = {
            .af = g->af, .mode = (uint8_t)g->mode, .otype = 0, .speed = 3, .pupd = 0
        };
        pm->fun->config(pm, g->port, g->pin, &cfg);
        return 0;                            /* pinmux owns the GPIO registers now */
    }

    /* Fallback when no pinmux is present: configure directly via the HAL. */
    gpio_hal_config(g->hal);
    return 0;
}

static int gpio_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int gpio_dev_read(device *self, void *buf, size_t len)
{
    gpio_pin *g = (gpio_pin *)self;
    if (len < 1) return -1;
    *(uint8_t *)buf = gpio_pin_read(g);
    return 1;
}

static int gpio_dev_write(device *self, const void *buf, size_t len)
{
    gpio_pin *g = (gpio_pin *)self;
    if (len < 1 || !buf) return -1;
    gpio_hal_write(g->hal, *(const uint8_t *)buf);
    return 1;
}

/* control-class ops — the REAL implementations; the base deviceVtable forwards
 * here. command() is the ioctl-style control surface; set/get unused -> -1. */
static int gpio_control_command(control_device *self, int cmd, void *arg)
{
    gpio_pin *g = (gpio_pin *)self;
    switch (cmd) {
    case GPIO_IOCTL_TOGGLE:
        gpio_pin_toggle(g);
        return 0;
    case GPIO_IOCTL_SET_MODE:
        if (!arg) return -1;
        gpio_hal_set_mode(g->hal, *(const uint32_t *)arg);
        return 0;
    default:
        return -1;
    }
}
static int gpio_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int gpio_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static const struct control_deviceVtable gpio_control_vtable = {
    .command = gpio_control_command,
    .set     = gpio_control_set,
    .get     = gpio_control_get,
};

/* base device-interface ops forward to the control-class vtable */
static int gpio_dev_ioctl(device *self, int cmd, void *arg)
    { return gpio_control_command((control_device *)self, cmd, arg); }
