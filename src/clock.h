#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

typedef struct _clock clock;

struct clockFun {
    void (*destroy)(clock *self);
    void (*init)(clock *self);
    void (*deinit)(clock *self);
    uint32_t (*get_sysclk_hz)(clock *self);
};

struct clockVtable {
    void (*configure)(clock *self);
};

struct _clock {
    struct clockVtable *vtable;
    const struct clockFun *fun;
    uint32_t sysclk_hz;
};

clock *clock_create(void);
void clock_destroy(clock *self);
void clock_init(clock *self);
void clock_deinit(clock *self);
uint32_t clock_get_sysclk_hz(clock *self);

extern const struct clockFun clock_fun;

#endif /* CLOCK_H */
