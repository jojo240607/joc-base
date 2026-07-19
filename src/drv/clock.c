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

/* subclass vtable (defined below; forward-declared so clock_init can reference it) */
static const struct control_deviceVtable clock_control_vtable;

/* public methods — `static`, reachable ONLY through self->fun-> */
static uint32_t clock_get_sysclk_hz(sys_clock *self);

const struct clockFun clock_fun = {
    .destroy = clock_destroy,
    .init    = clock_init,
    .deinit  = clock_deinit,
    .get_sysclk_hz = clock_get_sysclk_hz,
};

/* one shared vtable for the whole clock class — assigned by clock_init() */
static const struct deviceVtable clock_dev_vtable = {
    .open  = clock_dev_open,
    .close = clock_dev_close,
    .read  = clock_dev_read,
    .write = clock_dev_write,
    .ioctl = clock_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *clock_create(const void *config)
{
    const clock_config_t *c = (const clock_config_t *)config;
    sys_clock *self = (sys_clock *)malloc(sizeof(sys_clock));
    if (!self) return NULL;
    memset(self, 0, sizeof(sys_clock));
    self->parent.parent.type = DEVICE_TYPE_CLOCK;   /* driver sets its own class */
    self->parent.parent.name = c->name;             /* driver sets its own name */
    clock_init(self);
    return (device *)self;
}

void clock_destroy(sys_clock *self)
{
    if (!self) return;
    clock_deinit(self);
    free(self);
}

void clock_init(sys_clock *self)
{
    if (!self) return;
    self->parent.parent.vtable = &clock_dev_vtable;     /* base device vtable */
    self->parent.vtable        = &clock_control_vtable; /* control-class vtable */
    self->parent.parent.type   = DEVICE_TYPE_CLOCK;
    self->parent.parent.class  = DEVICE_CLASS_CONTROL;
    self->fun = &clock_fun;
    self->sysclk_hz = clock_hal_sysclk_hz();
    /* hardware bring-up (PLL) is deferred to open() (see clock_dev_open) */
}

void clock_deinit(sys_clock *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
}

static uint32_t clock_get_sysclk_hz(sys_clock *self)
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
    sys_clock *c = (sys_clock *)self;
    if (len < sizeof(uint32_t)) return -1;
    *(uint32_t *)buf = c->sysclk_hz;
    return (int)sizeof(uint32_t);
}

static int clock_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;   /* clock is not writable */
}

/* control-class ops — the REAL implementations; the base deviceVtable forwards
 * here. command() is the ioctl-style control surface; set/get are unused by the
 * clock (it is read-only state) so they return -1. */
static int clock_control_command(control_device *self, int cmd, void *arg)
{
    sys_clock *c = (sys_clock *)self;
    switch (cmd) {
    case CLK_IOCTL_GET_SYSCLK_HZ:
        if (!arg) return -1;
        *(uint32_t *)arg = c->sysclk_hz;
        return 0;
    default:
        return -1;
    }
}
static int clock_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int clock_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static const struct control_deviceVtable clock_control_vtable = {
    .command = clock_control_command,
    .set     = clock_control_set,
    .get     = clock_control_get,
};

/* base device-interface ops forward to the control-class vtable */
static int clock_dev_ioctl(device *self, int cmd, void *arg)
    { return clock_control_command((control_device *)self, cmd, arg); }
