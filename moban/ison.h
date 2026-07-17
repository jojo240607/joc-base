#ifndef ISON_H
#define ISON_H

#include "Ibase.h"

typedef struct _ison ison;

struct isonFun {
    void (*destroy)(ison *self);
    void (*ison_method)(ison *self);
    void (*ison_methods)(ison *self);
};

struct isonVtable {
    struct IbaseVtable parent;
    void (*ison_virtual_method)(ison *self);
    
};

struct _ison {
    union { Ibase parent; struct isonVtable *vtable; };
    const struct isonFun *fun;
};

ison *ison_create(void);
void ison_destroy(ison *self);
void ison_init(ison *self);
void ison_deinit(ison *self);

extern const struct isonFun ison_fun;

#endif /* ISON_H */
