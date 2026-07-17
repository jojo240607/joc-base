#include "base.h"
#include <stdlib.h>
#include <string.h>
static void base_base_method(base *self);
static void default_base_virtual_method_impl(base *self);

const struct baseFun base_fun = {
    .destroy = base_destroy,
    .base_method = base_base_method,
};

base *base_create(void) {
    base *self = (base*)malloc(sizeof(base));
    if (!self) return NULL;
    memset(self, 0, sizeof(base));
    base_init(self);
    return self;
}

void base_destroy(base *self) {
    if (!self) return;
    base_deinit(self);
    free(self);
}

void base_init(base *self) {

    /* OOC vtable allocation */
    if (!self->vtable) {
        self->vtable = (struct baseVtable*)malloc(sizeof(struct baseVtable));
        if (self->vtable) {
            memset(self->vtable, 0, sizeof(struct baseVtable));
        }
    }
    if (!self) return;
    self->fun = &base_fun;
    self->vtable->virtual_method = default_base_virtual_method_impl;
}

void base_deinit(base *self) {
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}


static void default_base_virtual_method_impl(base *self) {
    /* TODO: Implement */
    (void)self;
    
}


static void base_base_method(base *self) {
    /* TODO: Implement */
    
}
