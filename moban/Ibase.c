/* OOC_INTERFACE */
#include "Ibase.h"
#include <stdlib.h>
#include <string.h>
static void Ibase_ibase_method(Ibase *self);


static void default_Ibase_ibase_virtual_method_impl(Ibase *self) {
    /* TODO: default behavior, override in subclass */
    (void)self;
    
}

void Ibase_init(Ibase *self) {
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct IbaseVtable*)malloc(sizeof(struct IbaseVtable));
        if (self->vtable) {
            memset(self->vtable, 0, sizeof(struct IbaseVtable));
        }
    }
    self->vtable->ibase_virtual_method = default_Ibase_ibase_virtual_method_impl;

}

void Ibase_deinit(Ibase *self) {
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}


static void Ibase_ibase_method(Ibase *self) {
    /* TODO: Implement */
    
}


static void Ibase_ibase_method(Ibase *self) {
    /* TODO: Implement */
    
}


static void Ibase_ibase_method(Ibase *self) {
    /* TODO: Implement */
    
}
