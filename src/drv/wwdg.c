#include "wwdg.h"
#include "devmgr/device_manager.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int wwdg_dev_open(device *self);
static int wwdg_dev_close(device *self);
static int wwdg_dev_read(device *self, void *buf, size_t len);
static int wwdg_dev_write(device *self, const void *buf, size_t len);
static int wwdg_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (referenced by wwdg_init and the device vtable) */
static int wwdg_control_command(control_device *self, int cmd, void *arg);
static int wwdg_control_set(control_device *self, int param, const void *val);
static int wwdg_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable wwdg_control_vtable = {
    .command = wwdg_control_command,
    .set     = wwdg_control_set,
    .get     = wwdg_control_get,
};

static const struct deviceVtable wwdg_dev_vtable = {
    .open  = wwdg_dev_open,
    .close = wwdg_dev_close,
    .read  = wwdg_dev_read,
    .write = wwdg_dev_write,
    .ioctl = wwdg_dev_ioctl,
};

device *wwdg_create(const void *config)
{
    const wwdg_config_t *c = (const wwdg_config_t *)config;
    if (!c) return NULL;
    wwdg *p = (wwdg *)malloc(sizeof(wwdg));
    if (!p) return NULL;
    memset(p, 0, sizeof(wwdg));
    p->hal = wwdg_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.type   = DEVICE_TYPE_WWDG;    /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &wwdg_dev_vtable;    /* base device vtable */
    p->parent.vtable        = &wwdg_control_vtable;/* control-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;
    return (device *)p;
}

void wwdg_destroy(wwdg *self)
{
    if (!self) return;
    wwdg_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int wwdg_dev_open(device *self)
{
    wwdg *p = (wwdg *)self;
    wwdg_hal_enable(p->hal);   /* gate APB1 clock; does NOT arm the WDT */
    return 0;
}

static int wwdg_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* The WWDG is driven through ioctl(); raw read/write are not meaningful. */
static int wwdg_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int wwdg_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

/* --- control-class ops — the REAL implementations; the base deviceVtable
 *     forwards ioctl() here. set()/get() are unused (the ioctl command surface
 *     is richer), so they return -1. */
static int wwdg_control_command(control_device *self, int cmd, void *arg)
{
    wwdg *p = (wwdg *)self;
    switch (cmd) {
    case WWDG_IOCTL_SET_PRESCALER: {
        if (!arg) return -1;
        wwdg_hal_set_prescaler(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case WWDG_IOCTL_SET_WINDOW: {
        if (!arg) return -1;
        wwdg_hal_set_window(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case WWDG_IOCTL_GET_CONFIG: {
        if (!arg) return -1;
        *(uint32_t *)arg = wwdg_hal_get_config(p->hal);
        return 0;
    }
    case WWDG_IOCTL_START: {
        if (!arg) return -1;
        wwdg_hal_start(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case WWDG_IOCTL_REFRESH: {
        if (!arg) return -1;
        wwdg_hal_refresh(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case WWDG_IOCTL_GET_COUNTER: {
        if (!arg) return -1;
        *(uint32_t *)arg = wwdg_hal_get_counter(p->hal);
        return 0;
    }
    case WWDG_IOCTL_GET_STATUS: {
        if (!arg) return -1;
        *(uint32_t *)arg = wwdg_hal_get_status(p->hal);
        return 0;
    }
    default:
        return -1;
    }
}
static int wwdg_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int wwdg_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int wwdg_dev_ioctl(device *self, int cmd, void *arg)
    { return wwdg_control_command((control_device *)self, cmd, arg); }
