#include "rng.h"
#include "devmgr/device_manager.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int rng_dev_open(device *self);
static int rng_dev_close(device *self);
static int rng_dev_read(device *self, void *buf, size_t len);
static int rng_dev_write(device *self, const void *buf, size_t len);
static int rng_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (referenced by rng_init and the device vtable) */
static int rng_control_command(control_device *self, int cmd, void *arg);
static int rng_control_set(control_device *self, int param, const void *val);
static int rng_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable rng_control_vtable = {
    .command = rng_control_command,
    .set     = rng_control_set,
    .get     = rng_control_get,
};

static const struct deviceVtable rng_dev_vtable = {
    .open  = rng_dev_open,
    .close = rng_dev_close,
    .read  = rng_dev_read,
    .write = rng_dev_write,
    .ioctl = rng_dev_ioctl,
};

device *rng_create(const void *config)
{
    const rng_config_t *c = (const rng_config_t *)config;
    if (!c) return NULL;
    rng *p = (rng *)malloc(sizeof(rng));
    if (!p) return NULL;
    memset(p, 0, sizeof(rng));
    p->hal = rng_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.type   = DEVICE_TYPE_RNG;     /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &rng_dev_vtable;     /* base device vtable */
    p->parent.vtable        = &rng_control_vtable; /* control-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;
    return (device *)p;
}

void rng_destroy(rng *self)
{
    if (!self) return;
    rng_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int rng_dev_open(device *self)
{
    rng *p = (rng *)self;
    rng_hal_enable(p->hal);     /* clock gate + generator enable */
    return 0;
}

static int rng_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* A random stream: fill `buf` with `len` bytes of entropy (read as 32-bit
 * words). Returns the number of bytes written. */
static int rng_dev_read(device *self, void *buf, size_t len)
{
    rng *p = (rng *)self;
    if (!buf || len == 0U) return 0;
    uint8_t *dst = (uint8_t *)buf;
    size_t n = 0;
    while (n + sizeof(uint32_t) <= len) {
        uint32_t w = rng_hal_get_u32(p->hal);
        memcpy(dst + n, &w, sizeof(uint32_t));
        n += sizeof(uint32_t);
    }
    if (n < len) {                 /* trailing partial word */
        uint32_t w = rng_hal_get_u32(p->hal);
        memcpy(dst + n, &w, len - n);
        n = len;
    }
    return (int)n;
}

static int rng_dev_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len;
    return -1;   /* RNG is a source, not a sink */
}

/* --- control-class ops — the REAL implementations; the base deviceVtable
 *     forwards ioctl() here. set()/get() are unused (the ioctl command surface
 *     is richer), so they return -1. */
static int rng_control_command(control_device *self, int cmd, void *arg)
{
    rng *p = (rng *)self;
    switch (cmd) {
    case RNG_IOCTL_GET_U32: {
        if (!arg) return -1;
        *(uint32_t *)arg = rng_hal_get_u32(p->hal);
        return 0;
    }
    case RNG_IOCTL_GET_STATUS: {
        if (!arg) return -1;
        *(uint32_t *)arg = rng_hal_get_status(p->hal);
        return 0;
    }
    default:
        return -1;
    }
}
static int rng_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int rng_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int rng_dev_ioctl(device *self, int cmd, void *arg)
    { return rng_control_command((control_device *)self, cmd, arg); }
