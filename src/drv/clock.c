#include "clock.h"
#include "clock_hal.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int clock_dev_open(device *self);
static int clock_dev_close(device *self);
static int clock_dev_read(device *self, void *buf, size_t len);
static int clock_dev_write(device *self, const void *buf, size_t len);
static int clock_dev_ioctl(device *self, int cmd, void *arg);

/* public methods — `static`, reachable ONLY through self->fun-> */
static uint32_t clock_get_sysclk_hz(clock *self);

const struct clockFun clock_fun = {
    .destroy = clock_destroy,
    .init    = clock_init,
    .deinit  = clock_deinit,
    .get_sysclk_hz = clock_get_sysclk_hz,
};

clock *clock_create(void)
{
    clock *self = (clock *)malloc(sizeof(clock));
    if (!self) return NULL;
    memset(self, 0, sizeof(clock));
    clock_init(self);
    return self;
}

void clock_destroy(clock *self)
{
    if (!self) return;
    clock_deinit(self);
    free(self);
}

void clock_init(clock *self)
{
    if (!self) return;
    device_init(&self->parent);
    self->fun = &clock_fun;
    self->parent.vtable->open  = clock_dev_open;
    self->parent.vtable->close = clock_dev_close;
    self->parent.vtable->read  = clock_dev_read;
    self->parent.vtable->write = clock_dev_write;
    self->parent.vtable->ioctl = clock_dev_ioctl;
    self->sysclk_hz = clock_hal_sysclk_hz();
    self->parent.vtable->open((device *)self);   /* configure the PLL now */
}

void clock_deinit(clock *self)
{
    if (!self) return;
    device_deinit(&self->parent);
}

static uint32_t clock_get_sysclk_hz(clock *self)
{
    return self ? self->sysclk_hz : 0UL;
}

/* --- unified device-interface virtual implementations --- */

static int clock_dev_open(device *self)
{
    (void)self;
    clock_hal_configure();
    return 0;
}

static int clock_dev_close(device *self)
{
    (void)self;
    return 0;
}

static int clock_dev_read(device *self, void *buf, size_t len)
{
    clock *c = (clock *)self;
    if (len < sizeof(uint32_t)) return -1;
    *(uint32_t *)buf = c->sysclk_hz;
    return (int)sizeof(uint32_t);
}

static int clock_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;   /* clock is not writable */
}

static int clock_dev_ioctl(device *self, int cmd, void *arg)
{
    clock *c = (clock *)self;
    switch (cmd) {
    case CLK_IOCTL_GET_SYSCLK_HZ:
        if (!arg) return -1;
        *(uint32_t *)arg = c->sysclk_hz;
        return 0;
    default:
        return -1;
    }
}
