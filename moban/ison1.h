#ifndef ISON1_H
#define ISON1_H

#include "Ibase.h"

typedef struct _ison1 ison1;

struct ison1Fun {
    void (*destroy)(ison1 *self);
    void (*ison1_method)(ison1 *self);
};

struct _ison1 {
    Ibase parent;
    const struct ison1Fun *fun;
};

ison1 *ison1_create(void);
void ison1_destroy(ison1 *self);
void ison1_init(ison1 *self);
void ison1_deinit(ison1 *self);

extern const struct ison1Fun ison1_fun;

#endif /* ISON1_H */
