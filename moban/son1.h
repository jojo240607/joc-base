#ifndef SON1_H
#define SON1_H

#include "base.h"

typedef struct _son1 son1;

struct son1Fun {
    void (*destroy)(son1 *self);
    void (*son1_method)(son1 *self);
};

struct son1Vtable {
    struct baseVtable parent;
    void (*son_virtual_method)(son1 *self);
    
};

struct _son1 {
    union { base parent; struct son1Vtable *vtable; };
    const struct son1Fun *fun;
};

son1 *son1_create(void);
void son1_destroy(son1 *self);
void son1_init(son1 *self);
void son1_deinit(son1 *self);

extern const struct son1Fun son1_fun;

#endif /* SON1_H */
