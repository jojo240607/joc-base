#ifndef GPIO_PIN_H
#define GPIO_PIN_H

#include "stm32f4xx.h"
#include <stdint.h>

typedef struct _gpio_pin gpio_pin;

struct gpio_pinFun {
    void (*destroy)(gpio_pin *self);
    void (*init)(gpio_pin *self);
    void (*deinit)(gpio_pin *self);
    void (*set)(gpio_pin *self);
    void (*reset)(gpio_pin *self);
    void (*toggle)(gpio_pin *self);
};

struct gpio_pinVtable {
    void (*write)(gpio_pin *self, uint8_t state);
    uint8_t (*read)(gpio_pin *self);
};

struct _gpio_pin {
    struct gpio_pinVtable *vtable;
    const struct gpio_pinFun *fun;
    GPIO_TypeDef *port;
    uint32_t pin;     /* 0..15 */
    uint32_t mode;    /* 0=input, 1=output, 2=alternate */
};

gpio_pin *gpio_pin_create(GPIO_TypeDef *port, uint32_t pin, uint32_t mode);
void gpio_pin_destroy(gpio_pin *self);
void gpio_pin_init(gpio_pin *self);
void gpio_pin_deinit(gpio_pin *self);
void gpio_pin_set(gpio_pin *self);
void gpio_pin_reset(gpio_pin *self);
void gpio_pin_toggle(gpio_pin *self);

extern const struct gpio_pinFun gpio_pin_fun;

#endif /* GPIO_PIN_H */
