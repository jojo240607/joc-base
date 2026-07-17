#include "son1.h"
#include <stdlib.h>
#include <string.h>
static void son1_son1_method(son1 *self);
static void default_son1_son_virtual_method_impl(son1 *self);
static void override_son1_base_virtual_method_impl(base *self);

const struct son1Fun son1_fun = {
    .destroy = son1_destroy,
    .son1_method = son1_son1_method,
};

son1 *son1_create(void) {
    son1 *self = (son1*)malloc(sizeof(son1));
    if (!self) return NULL;
    memset(self, 0, sizeof(son1));
    son1_init(self);
    return self;
}

void son1_destroy(son1 *self) {
    if (!self) return;
    son1_deinit(self);
    free(self);
}

void son1_init(son1 *self) {

    /* OOC vtable allocation */
    if (!self->vtable) {
        self->vtable = (struct son1Vtable*)malloc(sizeof(struct son1Vtable));
        if (self->vtable) {
            memset(self->vtable, 0, sizeof(struct son1Vtable));
        }
    }
    if (!self) return;
    base_init(&self->parent);
    self->fun = &son1_fun;
    self->parent.vtable->virtual_method = override_son1_base_virtual_method_impl;
    self->vtable->son_virtual_method = default_son1_son_virtual_method_impl;
}

void son1_deinit(son1 *self) {
    if (!self) return;
    base_deinit(&self->parent);
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}


static void override_son1_base_virtual_method_impl(base *self) {
    son1 *child = (son1*)self;
    /* TODO: Override implementation */
    (void)child;
    
}


static void default_son1_son_virtual_method_impl(son1 *self) {
    /* TODO: Implement */
    (void)self;
    
}


static void son1_son1_method(son1 *self) {
    /* TODO: Implement */
    
}
