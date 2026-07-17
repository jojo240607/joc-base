#ifndef IBASE_H
#define IBASE_H

#include <stdint.h>

typedef struct _Ibase Ibase;

struct IbaseVtable {
    void (*ibase_virtual_method)(Ibase *self);
};

struct _Ibase {
    struct IbaseVtable *vtable;
};

void Ibase_init(Ibase *self);
void Ibase_deinit(Ibase *self);

#endif /* IBASE_H */
