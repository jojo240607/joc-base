#include "serial.h"
#include <stdlib.h>
#include <string.h>

const struct serialFun serial_fun = {
    .destroy = serial_destroy,
    .init = serial_init,
    .deinit = serial_deinit,
};

serial *serial_create(void)
{
    serial *self = (serial *)malloc(sizeof(serial));
    if (!self) return NULL;
    memset(self, 0, sizeof(serial));
    serial_init(self);
    return self;
}

void serial_destroy(serial *self)
{
    if (!self) return;
    serial_deinit(self);
    free(self);
}

void serial_init(serial *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct serialVtable *)malloc(sizeof(struct serialVtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct serialVtable));
    }
    self->fun = &serial_fun;
}

void serial_deinit(serial *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

void serial_putc(serial *self, char c)
{
    if (self && self->vtable && self->vtable->putc)
        self->vtable->putc(self, c);
}

void serial_puts(serial *self, const char *s)
{
    if (self && self->vtable && self->vtable->puts)
        self->vtable->puts(self, s);
}
