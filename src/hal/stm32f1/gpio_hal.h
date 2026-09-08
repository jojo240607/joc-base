#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — GPIO pin (STM32F1 implementation).
 *
 * The driver layer only handles the OPAQUE `gpio_hal_handle_t *`; it never sees
 * GPIO_TypeDef. The concrete struct (GPIO_TypeDef*, pin, mode) is private to
 * gpio_hal.c. Porting to another chip = rewrite this HAL only.
 *
 * STM32F1 GPIO uses CRL/CRH registers instead of F4's MODER/OTYPER/OSPEEDR/PUPDR.
 */
typedef struct gpio_hal_handle gpio_hal_handle_t;

/* board passes the real port (void*), the pin and the logical mode (0 in/1 out/2 alt) */
gpio_hal_handle_t *gpio_hal_create(void *peripheral, uint32_t pin, uint32_t mode);
void gpio_hal_destroy(gpio_hal_handle_t *h);

void gpio_hal_config(gpio_hal_handle_t *h);          /* (re)configure from handle state */
void gpio_hal_set(gpio_hal_handle_t *h);
void gpio_hal_reset(gpio_hal_handle_t *h);
void gpio_hal_toggle(gpio_hal_handle_t *h);
uint8_t gpio_hal_read(gpio_hal_handle_t *h);
void gpio_hal_write(gpio_hal_handle_t *h, uint8_t state);
void gpio_hal_set_mode(gpio_hal_handle_t *h, uint32_t mode); /* update mode + reconfigure */

#endif /* GPIO_HAL_H */