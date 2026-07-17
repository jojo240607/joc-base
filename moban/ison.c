#include "ison.h"
#include <stdlib.h>
#include <string.h>
static void ison_ison_methods(ison *self);
static void ison_ison_method(ison *self);
static void default_ison_ison_virtual_method_impl(ison *self);
static void override_ison_Ibase_ibase_virtual_method_impl(Ibase *self);

const struct isonFun ison_fun = {
    .destroy = ison_destroy,
    .ison_method = ison_ison_method,
    .ison_methods = ison_ison_methods,
};

ison *ison_create(void) {
    ison *self = (ison*)malloc(sizeof(ison));
    if (!self) return NULL;
    memset(self, 0, sizeof(ison));
    ison_init(self);
    return self;
}

void ison_destroy(ison *self) {
    if (!self) return;
    ison_deinit(self);
    free(self);
}

void ison_init(ison *self) {

    /* OOC vtable allocation */
    if (!self->vtable) {
        self->vtable = (struct isonVtable*)malloc(sizeof(struct isonVtable));
        if (self->vtable) {
            memset(self->vtable, 0, sizeof(struct isonVtable));
        }
    }
    if (!self) return;
    Ibase_init(&self->parent);
    self->fun = &ison_fun;
    self->parent.vtable->ibase_virtual_method = override_ison_Ibase_ibase_virtual_method_impl;
    self->vtable->ison_virtual_method = default_ison_ison_virtual_method_impl;
}

void ison_deinit(ison *self) {
    if (!self) return;
    Ibase_deinit(&self->parent);
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}


static void override_ison_Ibase_ibase_virtual_method_impl(Ibase *self) {
    ison *child = (ison*)self;
    /* TODO: Override implementation */
    (void)child;
    
}


static void default_ison_ison_virtual_method_impl(ison *self) {
    /* TODO: Implement */
    (void)self;
    
}


static void ison_ison_method(ison *self) {
    /* TODO: Implement */
    
}


static void ison_ison_methods(ison *self) {
    /* TODO: Implement */
    
}
