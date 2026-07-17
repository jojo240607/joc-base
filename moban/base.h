#ifndef BASE_H
#define BASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct _base base;

struct baseFun {
    void (*destroy)(base *self);
    void (*base_method)(base *self);
};

struct baseVtable {
    void (*virtual_method)(base *self);
    
};

struct _base {
    struct baseVtable *vtable;
    const struct baseFun *fun;
    /* TODO: Add member variables here */
};

base *base_create(void);
void base_destroy(base *self);
void base_init(base *self);
void base_deinit(base *self);

extern const struct baseFun base_fun;

#endif /* BASE_H */
