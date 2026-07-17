#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

typedef struct _serial serial;

struct serialFun {
    void (*destroy)(serial *self);
    void (*init)(serial *self);
    void (*deinit)(serial *self);
};

struct serialVtable {
    void (*putc)(serial *self, char c);
    void (*puts)(serial *self, const char *s);
    char (*getc)(serial *self);   /* blocking receive (overridden by uart) */
};

struct _serial {
    struct serialVtable *vtable;
    const struct serialFun *fun;
};

void serial_init(serial *self);
void serial_deinit(serial *self);
void serial_destroy(serial *self);
void serial_putc(serial *self, char c);
void serial_puts(serial *self, const char *s);
char serial_getc(serial *self);

extern const struct serialFun serial_fun;

#endif /* SERIAL_H */
