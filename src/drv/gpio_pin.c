#include "gpio_pin.h"
#include <stdlib.h>
#include <string.h>

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

gpio_pin *gpio_pin_create(gpio_hal_handle_t *hal, const char *name)
{
    gpio_pin *self = (gpio_pin *)malloc(sizeof(gpio_pin));
    if (!self) return NULL;
    memset(self, 0, sizeof(gpio_pin));
    self->hal = hal;
    self->parent.type = DEVICE_TYPE_GPIO;    /* driver sets its own class */
    self->parent.name = name;                /* driver sets its own name */
    gpio_pin_init(self);
    return self;
}

void gpio_pin_destroy(gpio_pin *self)
{
    if (!self) return;
    gpio_pin_deinit(self);
    free(self);
}

void gpio_pin_init(gpio_pin *self)
{
    if (!self) return;
    device_init(&self->parent);
    self->fun = &gpio_pin_fun;
    self->parent.vtable->open  = gpio_dev_open;
    self->parent.vtable->close = gpio_dev_close;
    self->parent.vtable->read  = gpio_dev_read;
    self->parent.vtable->write = gpio_dev_write;
    self->parent.vtable->ioctl = gpio_dev_ioctl;
    gpio_hal_config(self->hal);
}

void gpio_pin_deinit(gpio_pin *self)
{
    if (!self) return;
    device_deinit(&self->parent);
}

static void gpio_pin_set(gpio_pin *self)   { if (self) gpio_hal_set(self->hal); }
static void gpio_pin_reset(gpio_pin *self) { if (self) gpio_hal_reset(self->hal); }
static void gpio_pin_toggle(gpio_pin *self){ if (self) gpio_hal_toggle(self->hal); }
static uint8_t gpio_pin_read(gpio_pin *self){ return self ? gpio_hal_read(self->hal) : 0U; }

/* --- unified device-interface virtual implementations --- */

static int gpio_dev_open(device *self)
{
    gpio_hal_config(((gpio_pin *)self)->hal);
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
