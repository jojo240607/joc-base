#include "iwdg.h"
#include "devmgr/device_manager.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int iwdg_dev_open(device *self);
static int iwdg_dev_close(device *self);
static int iwdg_dev_read(device *self, void *buf, size_t len);
static int iwdg_dev_write(device *self, const void *buf, size_t len);
static int iwdg_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (referenced by iwdg_init and the device vtable) */
static int iwdg_control_command(control_device *self, int cmd, void *arg);
static int iwdg_control_set(control_device *self, int param, const void *val);
static int iwdg_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable iwdg_control_vtable = {
    .command = iwdg_control_command,
    .set     = iwdg_control_set,
    .get     = iwdg_control_get,
};

static const struct deviceVtable iwdg_dev_vtable = {
    .open  = iwdg_dev_open,
    .close = iwdg_dev_close,
    .read  = iwdg_dev_read,
    .write = iwdg_dev_write,
    .ioctl = iwdg_dev_ioctl,
};

device *iwdg_create(const void *config)
{
    const iwdg_config_t *c = (const iwdg_config_t *)config;
    if (!c) return NULL;
    iwdg *p = (iwdg *)malloc(sizeof(iwdg));
    if (!p) return NULL;
    memset(p, 0, sizeof(iwdg));
    p->hal = iwdg_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.type   = DEVICE_TYPE_IWDG;    /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &iwdg_dev_vtable;    /* base device vtable */
    p->parent.vtable        = &iwdg_control_vtable;/* control-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;
    return (device *)p;
}

void iwdg_destroy(iwdg *self)
{
    if (!self) return;
    iwdg_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int iwdg_dev_open(device *self)
{
    iwdg *p = (iwdg *)self;
    iwdg_hal_enable(p->hal);   /* ensure LSI is running; does NOT arm the WDT */
    return 0;
}

static int iwdg_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* The IWDG is driven through ioctl(); raw read/write are not meaningful. */
static int iwdg_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int iwdg_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

/* --- control-class ops — the REAL implementations; the base deviceVtable
 *     forwards ioctl() here. set()/get() are unused (the ioctl command surface
 *     is richer), so they return -1. */
static int iwdg_control_command(control_device *self, int cmd, void *arg)
{
    iwdg *p = (iwdg *)self;
    switch (cmd) {
    case IWDG_IOCTL_SET_PRESCALER: {
        if (!arg) return -1;
        iwdg_hal_set_prescaler(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case IWDG_IOCTL_SET_RELOAD: {
        if (!arg) return -1;
        iwdg_hal_set_reload(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case IWDG_IOCTL_GET_PRESCALER: {
        if (!arg) return -1;
        *(uint32_t *)arg = iwdg_hal_get_prescaler(p->hal);
        return 0;
    }
    case IWDG_IOCTL_GET_RELOAD: {
        if (!arg) return -1;
        *(uint32_t *)arg = iwdg_hal_get_reload(p->hal);
        return 0;
    }
    case IWDG_IOCTL_START:
        iwdg_hal_start(p->hal);
        return 0;
    case IWDG_IOCTL_REFRESH:
        iwdg_hal_refresh(p->hal);
        return 0;
    case IWDG_IOCTL_GET_STATUS: {
        if (!arg) return -1;
        *(uint32_t *)arg = iwdg_hal_get_status(p->hal);
        return 0;
    }
    default:
        return -1;
    }
}
static int iwdg_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int iwdg_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int iwdg_dev_ioctl(device *self, int cmd, void *arg)
    { return iwdg_control_command((control_device *)self, cmd, arg); }
