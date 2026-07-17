#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdint.h>
#include "clock.h"
#include "uart_stm32.h"
#include "gpio_pin.h"
#include "adc_stm32.h"
#include "temp_sensor_stm32.h"

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
    clock *clk;
    uart_stm32 *uart;
    gpio_pin *led;
    adc_stm32 *adc;
    temp_sensor_stm32 *temp;
};

selftest *selftest_create(clock *clk, uart_stm32 *uart, gpio_pin *led,
                          adc_stm32 *adc, temp_sensor_stm32 *temp);
void selftest_destroy(selftest *self);
void selftest_init(selftest *self);
void selftest_deinit(selftest *self);
int  selftest_run(selftest *self);

extern const struct selftestFun selftest_fun;

#endif /* SELFTEST_H */
