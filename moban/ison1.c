#include "ison1.h"
#include <stdlib.h>
#include <string.h>
static void ison1_ison1_method(ison1 *self);
static void override_ison1_Ibase_ibase_virtual_method_impl(Ibase *self);

const struct ison1Fun ison1_fun = {
    .destroy = ison1_destroy,
    .ison1_method = ison1_ison1_method,
};

ison1 *ison1_create(void) {
    ison1 *self = (ison1*)malloc(sizeof(ison1));
    if (!self) return NULL;
    memset(self, 0, sizeof(ison1));
    ison1_init(self);
    return self;
}

void ison1_destroy(ison1 *self) {
    if (!self) return;
    ison1_deinit(self);
    free(self);
}

void ison1_init(ison1 *self) {
    if (!self) return;
    Ibase_init(&self->parent);
    self->fun = &ison1_fun;
    self->parent.vtable->ibase_virtual_method = override_ison1_Ibase_ibase_virtual_method_impl;
}

void ison1_deinit(ison1 *self) {
    if (!self) return;
    Ibase_deinit(&self->parent);
}


static void override_ison1_Ibase_ibase_virtual_method_impl(Ibase *self) {
    ison1 *child = (ison1*)self;
    /* TODO: Override implementation */
    (void)child;
    
}


static void ison1_ison1_method(ison1 *self) {
    /* TODO: Implement */
    
}


