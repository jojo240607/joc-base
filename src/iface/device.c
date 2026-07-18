/* Unified driver interface — vtable allocation and default (no-op /
 * unsupported) virtual implementations. Pure interface: no chip or driver
 * knowledge here. Default virtual bodies are `static` (never in the header). */
#include "device.h"
#include <stdlib.h>
#include <string.h>

/* default virtual implementations — `static`, defined only in this .c */
static int device_default_open(device *self)  { (void)self; return 0; }
static int device_default_close(device *self) { (void)self; return 0; }
static int device_default_read(device *self, void *buf, size_t len)
{
    (void)self; (void)buf; (void)len; return -1;
}
static int device_default_write(device *self, const void *buf, size_t len)
{
    (void)self; (void)buf; (void)len; return -1;
}
static int device_default_ioctl(device *self, int cmd, void *arg)
{
    (void)self; (void)cmd; (void)arg; return -1;
}

void device_init(device *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct deviceVtable *)malloc(sizeof(struct deviceVtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct deviceVtable));
    }
    if (!self->vtable) return;
    /* safe defaults so an unimplemented op never crashes the caller */
    self->vtable->open  = device_default_open;
    self->vtable->close = device_default_close;
    self->vtable->read  = device_default_read;
    self->vtable->write = device_default_write;
    self->vtable->ioctl = device_default_ioctl;
}

void device_deinit(device *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}
