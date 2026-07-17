#include "gpio_pin.h"
#include <stdlib.h>
#include <string.h>

static void gpio_pin_vwrite(gpio_pin *self, uint8_t state);
static uint8_t gpio_pin_vread(gpio_pin *self);

const struct gpio_pinFun gpio_pin_fun = {
    .destroy = gpio_pin_destroy,
    .init = gpio_pin_init,
    .deinit = gpio_pin_deinit,
    .set = gpio_pin_set,
    .reset = gpio_pin_reset,
    .toggle = gpio_pin_toggle,
};

gpio_pin *gpio_pin_create(GPIO_TypeDef *port, uint32_t pin, uint32_t mode)
{
    gpio_pin *self = (gpio_pin *)malloc(sizeof(gpio_pin));
    if (!self) return NULL;
    memset(self, 0, sizeof(gpio_pin));
    self->port = port;
    self->pin = pin;
    self->mode = mode;
    gpio_pin_init(self);
    return self;
}

void gpio_pin_destroy(gpio_pin *self)
{
    if (!self) return;
    gpio_pin_deinit(self);
    free(self);
}

void gpio_pin_init(gpio_pin *self)
{
    if (!self) return;
    if (!self->vtable) {
        self->vtable = (struct gpio_pinVtable *)malloc(sizeof(struct gpio_pinVtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct gpio_pinVtable));
    }
    self->fun = &gpio_pin_fun;
    self->vtable->write = gpio_pin_vwrite;
    self->vtable->read = gpio_pin_vread;

    /* enable the GPIO port clock */
    if (self->port == GPIOA)      RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (self->port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (self->port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (self->port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (self->port == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
    else if (self->port == GPIOH) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOHEN;

    /* configure pin mode */
    self->port->MODER = (self->port->MODER & ~(3U << (self->pin * 2)))
                      | (self->mode << (self->pin * 2));
    if (self->mode == 1U) {                 /* output */
        self->port->OTYPER &= ~(1U << self->pin);
        self->port->OSPEEDR |= (3U << (self->pin * 2));
        self->port->PUPDR   &= ~(3U << (self->pin * 2));
    }
}

void gpio_pin_deinit(gpio_pin *self)
{
    if (!self) return;
    if (self->vtable) {
        free(self->vtable);
        self->vtable = NULL;
    }
}

void gpio_pin_set(gpio_pin *self)
{
    if (self) self->port->BSRR = (1U << self->pin);
}

void gpio_pin_reset(gpio_pin *self)
{
    if (self) self->port->BSRR = (1U << (self->pin + 16));
}

void gpio_pin_toggle(gpio_pin *self)
{
    if (self) self->port->ODR ^= (1U << self->pin);
}

uint8_t gpio_pin_read(gpio_pin *self)
{
    if (!self) return 0U;
    return (self->port->IDR & (1U << self->pin)) ? 1U : 0U;
}

static void gpio_pin_vwrite(gpio_pin *self, uint8_t state)
{
    if (state) gpio_pin_set(self);
    else       gpio_pin_reset(self);
}

static uint8_t gpio_pin_vread(gpio_pin *self)
{
    return (self->port->IDR & (1U << self->pin)) ? 1U : 0U;
}
