#ifndef SON_H
#define SON_H

#include "base.h"

typedef struct _son son;

struct sonFun {
    void (*destroy)(son *self);
    void (*son_method)(son *self);
};

struct _son {
    base parent;
    const struct sonFun *fun;
};

son *son_create(void);
void son_destroy(son *self);
void son_init(son *self);
void son_deinit(son *self);

extern const struct sonFun son_fun;

#endif /* SON_H */
