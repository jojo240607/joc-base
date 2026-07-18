#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdint.h>
#include "iface/device.h"
#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"

typedef struct _selftest selftest;

struct selftestFun {
    void (*destroy)(selftest *self);
    void (*init)(selftest *self);
    void (*deinit)(selftest *self);
    int  (*run)(selftest *self);   /* 1 = all sub-tests passed */
};

struct selftestVtable {
    int (*test_clock)(selftest *self);  /* returns 1 if pass */
    int (*test_uart)(selftest *self);
    int (*test_gpio)(selftest *self);
    int (*test_adc)(selftest *self);
    int (*test_temp)(selftest *self);
};

struct _selftest {
    struct selftestVtable *vtable;
    const struct selftestFun *fun;
    /* The self-test drives every peripheral through the SAME unified
     * `device *` interface — exactly like the application layer. */
    device *clk;
    device *uart;
    device *led;
    device *adc;
    device *temp;
};

selftest *selftest_create(clock *clk, uart *uart, gpio_pin *led,
                          adc *adc, temp_sensor *temp);
void selftest_destroy(selftest *self);
void selftest_init(selftest *self);
void selftest_deinit(selftest *self);
int  selftest_run(selftest *self);

extern const struct selftestFun selftest_fun;

#endif /* SELFTEST_H */
