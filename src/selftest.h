#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdint.h>
#include "iface/device.h"

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
    int (*test_io)(selftest *self);     /* unified sync/async transfer API */
    int (*test_mode)(selftest *self);   /* POLL/IRQ engine switching + ADC IRQ read */
    int (*test_timer)(selftest *self);  /* general-purpose TIM periodic EVENT */
    int (*test_pwm)(selftest *self);    /* TIM compare channel as PWM output */
    int (*test_exti)(selftest *self);   /* GPIO pin external interrupt (EVENT) */
    int (*test_adv_timer)(selftest *self); /* advanced TIM: repetition counter (RCR) */
    int (*test_adv_pwm)(selftest *self);   /* advanced TIM PWM: MOE/dead-time/comp */
    int (*test_i2c)(selftest *self);       /* I2C master: config readback + bus scan */
    int (*test_spi)(selftest *self);       /* SPI master: config readback + xfer */
    int (*test_sdio)(selftest *self);      /* SDIO: register readback */
    int (*test_dac)(selftest *self);       /* DAC: value-path + channel enable */
    int (*test_rtc)(selftest *self);       /* RTC: calendar read/write + prescaler */
    int (*test_rng)(selftest *self);       /* RNG: entropy source + error status */
    int (*test_crc)(selftest *self);       /* CRC: checksum engine correctness */
};

struct _selftest {
    const struct selftestVtable *vtable;
    const struct selftestFun *fun;
    /* The self-test drives every peripheral through the SAME unified
     * `device *` interface — exactly like the application layer. */
    device *clk;
    device *uart;
    device *led;
    device *adc;
    device *temp;
};

/* Takes ONLY unified `device *` handles (resolved by name from the device
 * manager) — no concrete driver types, so the self-test stays decoupled. */
selftest *selftest_create(device *clk, device *uart, device *led,
                          device *adc, device *temp);
void selftest_destroy(selftest *self);
void selftest_init(selftest *self);
void selftest_deinit(selftest *self);
int  selftest_run(selftest *self);

extern const struct selftestFun selftest_fun;

#endif /* SELFTEST_H */
