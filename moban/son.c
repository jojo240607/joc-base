#include "son.h"
#include <stdlib.h>
#include <string.h>
static void son_son_method(son *self);
static void override_son_base_virtual_method_impl(base *self);

const struct sonFun son_fun = {
    .destroy = son_destroy,
    .son_method = son_son_method,
};

son *son_create(void) {
    son *self = (son*)malloc(sizeof(son));
    if (!self) return NULL;
    memset(self, 0, sizeof(son));
    son_init(self);
    return self;
}

void son_destroy(son *self) {
    if (!self) return;
    son_deinit(self);
    free(self);
}

void son_init(son *self) {
    if (!self) return;
    base_init(&self->parent);
    self->fun = &son_fun;
    self->parent.vtable->virtual_method = override_son_base_virtual_method_impl;
}

void son_deinit(son *self) {
    if (!self) return;
    base_deinit(&self->parent);
}


static void override_son_base_virtual_method_impl(base *self) {
    son *child = (son*)self;
    /* TODO: Override implementation */
    (void)child;
    
}


static void son_son_method(son *self) {
    /* TODO: Implement */
    
}
