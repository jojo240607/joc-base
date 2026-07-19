#ifndef GPIO_PIN_H
#define GPIO_PIN_H

#include "iface/device.h"
#include "iface/control_device.h"  /* gpio pin IS-A control_device (output/state) */
#include "gpio_hal.h"         /* opaque handle ONLY — no STM32 types reach the driver */
#include "pinmux_hal.h"       /* pinmux_port_t (port index for the pinmux) */
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
    control_device parent;        /* unified interface — MUST be first member (IS-A control_device) */
    const struct gpio_pinFun *fun;
    gpio_hal_handle_t *hal;       /* opaque — driver never dereferences it */
    uint32_t mode;                /* cached direction (0=in,1=out,2=alt) */
    const char *signal;           /* cached signal name (e.g. "GPIOD_12") */
    pinmux_port_t port;           /* resolved port for pinmux claim + HAL */
    uint8_t pin;                  /* resolved pin for pinmux claim + HAL */
    uint8_t af;                   /* resolved af (0 for plain GPIO) */
};

device *gpio_pin_create(const void *config);
void gpio_pin_destroy(gpio_pin *self);
void gpio_pin_init(gpio_pin *self);
void gpio_pin_deinit(gpio_pin *self);

/* Driver-specific board config — defined HERE (driver layer), filled by the
 * board. The board layer instantiates this as DATA; gpio_pin_create() reads it. */
typedef struct {
    const char *name;       /* logical device name */
    const char *signal;     /* pin signal name, e.g. "GPIOD_12" (resolved to
                               the exact port/pin by the pinmux at create/open) */
    uint32_t mode;          /* 0 = in, 1 = out, 2 = alt */
} gpio_config_t;

extern const struct gpio_pinFun gpio_pin_fun;

#endif /* GPIO_PIN_H */
