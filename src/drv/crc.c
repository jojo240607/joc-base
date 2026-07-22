#include "crc.h"
#include "devmgr/device_manager.h"
#include <stdlib.h>
#include <string.h>

/* virtual implementations dispatched through the unified device vtable */
static int crc_dev_open(device *self);
static int crc_dev_close(device *self);
static int crc_dev_read(device *self, void *buf, size_t len);
static int crc_dev_write(device *self, const void *buf, size_t len);
static int crc_dev_ioctl(device *self, int cmd, void *arg);

/* control-class vtable (referenced by crc_init and the device vtable) */
static int crc_control_command(control_device *self, int cmd, void *arg);
static int crc_control_set(control_device *self, int param, const void *val);
static int crc_control_get(control_device *self, int param, void *val);

static const struct control_deviceVtable crc_control_vtable = {
    .command = crc_control_command,
    .set     = crc_control_set,
    .get     = crc_control_get,
};

static const struct deviceVtable crc_dev_vtable = {
    .open  = crc_dev_open,
    .close = crc_dev_close,
    .read  = crc_dev_read,
    .write = crc_dev_write,
    .ioctl = crc_dev_ioctl,
};

device *crc_create(const void *config)
{
    const crc_config_t *c = (const crc_config_t *)config;
    if (!c) return NULL;
    crc *p = (crc *)malloc(sizeof(crc));
    if (!p) return NULL;
    memset(p, 0, sizeof(crc));
    p->hal = crc_hal_create(c->periph);
    if (!p->hal) { free(p); return NULL; }
    p->parent.parent.type   = DEVICE_TYPE_CRC;     /* driver sets its own class */
    p->parent.parent.name   = c->name;             /* driver sets its own name */
    p->parent.parent.vtable = &crc_dev_vtable;     /* base device vtable */
    p->parent.vtable        = &crc_control_vtable; /* control-class vtable */
    p->parent.parent.class  = DEVICE_CLASS_CONTROL;
    return (device *)p;
}

void crc_destroy(crc *self)
{
    if (!self) return;
    crc_hal_destroy(self->hal);
    free(self);
}

/* --- unified device-interface virtual implementations --- */
static int crc_dev_open(device *self)
{
    crc *p = (crc *)self;
    crc_hal_enable(p->hal);     /* clock gate */
    return 0;
}

static int crc_dev_close(device *self)
{
    (void)self;
    return 0;
}

/* The CRC is driven through ioctl(); raw read/write are not meaningful. */
static int crc_dev_read(device *self, void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }
static int crc_dev_write(device *self, const void *buf, size_t len)
    { (void)self; (void)buf; (void)len; return -1; }

/* --- control-class ops — the REAL implementations; the base deviceVtable
 *     forwards ioctl() here. set()/get() are unused (the ioctl command surface
 *     is richer), so they return -1. */
static int crc_control_command(control_device *self, int cmd, void *arg)
{
    crc *p = (crc *)self;
    switch (cmd) {
    case CRC_IOCTL_RESET: {
        crc_hal_reset(p->hal);
        return 0;
    }
    case CRC_IOCTL_UPDATE: {
        if (!arg) return -1;
        crc_hal_update_u32(p->hal, *(const uint32_t *)arg);
        return 0;
    }
    case CRC_IOCTL_RESULT: {
        if (!arg) return -1;
        *(uint32_t *)arg = crc_hal_result(p->hal);
        return 0;
    }
    default:
        return -1;
    }
}
static int crc_control_set(control_device *self, int param, const void *val)
    { (void)self; (void)param; (void)val; return -1; }
static int crc_control_get(control_device *self, int param, void *val)
    { (void)self; (void)param; (void)val; return -1; }

static int crc_dev_ioctl(device *self, int cmd, void *arg)
    { return crc_control_command((control_device *)self, cmd, arg); }
