#include "gpio_pin.h"
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */

/* virtual implementations dispatched through the unified device vtable */
static int gpio_dev_open(device *self);
static int gpio_dev_close(device *self);
static int gpio_dev_read(device *self, void *buf, size_t len);
static int gpio_dev_write(device *self, const void *buf, size_t len);
static int gpio_dev_ioctl(device *self, int cmd, void *arg);

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
    self->hal = gpio_hal_create(c->periph, c->pin, c->mode);
    if (!self->hal) { free(self); return NULL; }   /* #9: HAL alloc failure */
    self->mode = c->mode;                    /* cache for pinmux config at open() */
    self->signal = c->signal;                /* cache signal name for open() */
    self->parent.type = DEVICE_TYPE_GPIO;    /* driver sets its own class */
    self->parent.name = c->name;             /* driver sets its own name */
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
    self->parent.vtable = &gpio_dev_vtable;   /* per-class shared vtable */
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
     * A conflict (pin already owned elsewhere) makes request_signal fail and we
     * refuse to touch the hardware. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm && g->signal) {
        if (pm->fun->request_signal(pm, g->signal, g->parent.name) != 0) {
            printf("[gpio] %s: pin %s CONFLICT — refused\r\n", g->parent.name, g->signal);
            return -2;                       /* conflict: do NOT configure */
        }
        pinmux_pin_cfg_t cfg = {
            .af = 0, .mode = (uint8_t)g->mode, .otype = 0, .speed = 3, .pupd = 0
        };
        pinmux_port_t p; uint8_t n, a;
        if (pinmux_hal_resolve(g->signal, &p, &n, &a) == 1)
            pm->fun->config(pm, p, n, &cfg);
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

static int gpio_dev_ioctl(device *self, int cmd, void *arg)
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
