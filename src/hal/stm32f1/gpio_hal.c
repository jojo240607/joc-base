#include "gpio_hal.h"
#include "stm32f1xx.h"
#include <stdlib.h>

/* OPAQUE handle — the only GPIO state the HAL keeps. Hidden from the driver. */
struct gpio_hal_handle {
    GPIO_TypeDef *port;
    uint32_t pin;
    uint32_t mode;   /* 0 = input, 1 = output, 2 = alternate */
};

gpio_hal_handle_t *gpio_hal_create(void *peripheral, uint32_t pin, uint32_t mode)
{
    gpio_hal_handle_t *h = (gpio_hal_handle_t *)malloc(sizeof(gpio_hal_handle_t));
    if (!h) return NULL;
    h->port = (GPIO_TypeDef *)peripheral;
    h->pin  = pin;
    h->mode = mode;
    return h;
}

void gpio_hal_destroy(gpio_hal_handle_t *h)
{
    free(h);
}

void gpio_hal_config(gpio_hal_handle_t *h)
{
    /* The pin is claimed AND configured by the driver through the pinmux
     * (drv/pinmux.c -> hal/stm32f1/pinmux_hal.c) at open() time, so the GPIO
     * registers must NOT be written here — doing so would bypass the conflict
     * arbitrator. This function is kept only as a documentation/no-op anchor. */
    (void)h;
}

/* F1 BSRR: lower 16 bits = set, upper 16 bits = reset (same as F4) */
void gpio_hal_set(gpio_hal_handle_t *h)   { if (h && h->port) h->port->BSRR = (1U << h->pin); }
void gpio_hal_reset(gpio_hal_handle_t *h) { if (h && h->port) h->port->BSRR = (1U << (h->pin + 16)); }
void gpio_hal_toggle(gpio_hal_handle_t *h){ if (h && h->port) h->port->ODR ^= (1U << h->pin); }

uint8_t gpio_hal_read(gpio_hal_handle_t *h)
{
    if (!h || !h->port) return 0U;
    return (h->port->IDR & (1U << h->pin)) ? 1U : 0U;
}

void gpio_hal_write(gpio_hal_handle_t *h, uint8_t state)
{
    if (!h || !h->port) return;
    if (state) gpio_hal_set(h);
    else       gpio_hal_reset(h);
}

void gpio_hal_set_mode(gpio_hal_handle_t *h, uint32_t mode)
{
    if (!h) return;
    h->mode = mode;
    gpio_hal_config(h);
}