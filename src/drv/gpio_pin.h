#ifndef GPIO_PIN_H
#define GPIO_PIN_H

#include "iface/device.h"
#include "gpio_hal.h"         /* opaque handle ONLY — no STM32 types reach the driver */
#include <stdint.h>

/* device-level control commands for the GPIO driver */
#define GPIO_IOCTL_TOGGLE   0x01   /* arg: NULL */
#define GPIO_IOCTL_SET_MODE 0x02   /* arg: const uint32_t* mode (0=in,1=out,2=alt) */

/*
 * Driver layer — generic GPIO pin. Platform-independent: holds ONLY an opaque
 * `gpio_hal_handle_t *`. The board creates the handle (gpio_hal_create) with the
 * real port/pin/mode, so this driver never sees GPIO_TypeDef. Switching chips =
 * rewrite hal/<new-platform>/gpio_hal only. Implements the unified `device`.
 */
typedef struct _gpio_pin gpio_pin;

struct gpio_pinFun {
    void (*destroy)(gpio_pin *self);
    void (*init)(gpio_pin *self);
    void (*deinit)(gpio_pin *self);
    void (*set)(gpio_pin *self);       /* concrete convenience methods */
    void (*reset)(gpio_pin *self);
    void (*toggle)(gpio_pin *self);
    uint8_t (*read)(gpio_pin *self);
};

struct _gpio_pin {
    device parent;                /* unified interface — MUST be first member */
    const struct gpio_pinFun *fun;
    gpio_hal_handle_t *hal;       /* opaque — driver never dereferences it */
};

gpio_pin *gpio_pin_create(gpio_hal_handle_t *hal, const char *name);
void gpio_pin_destroy(gpio_pin *self);
void gpio_pin_init(gpio_pin *self);
void gpio_pin_deinit(gpio_pin *self);

extern const struct gpio_pinFun gpio_pin_fun;

#endif /* GPIO_PIN_H */
