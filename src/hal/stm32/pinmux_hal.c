#include "pinmux_hal.h"
#include "stm32f4xx.h"
#include <stdlib.h>

/*
 * Low-level GPIO register programming for the pinmux. Uses the CONTIGUOUS GPIO
 * register-block layout of the STM32F4 family (GPIOA @ 0x40020000, each port
 * 0x400 apart) so we never reference a GPIOx symbol that might be absent on a
 * particular package — only GPIO_TypeDef (layout) and RCC are needed.
 */
#define GPIOA_BASE_ADDR 0x40020000UL
#define GPIO_PORT_STRIDE 0x400UL

static GPIO_TypeDef *port_base(pinmux_port_t port)
{
    if (port >= PINMUX_PORT_COUNT) return NULL;
    return (GPIO_TypeDef *)(GPIOA_BASE_ADDR + (uint32_t)port * GPIO_PORT_STRIDE);
}

void *pinmux_hal_port_base(pinmux_port_t port)
{
    return (void *)port_base(port);
}

void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg)
{
    if (!cfg) return;
    GPIO_TypeDef *gpio = port_base(port);
    if (!gpio || pin >= 16U) return;

    /* Enable the port clock. On STM32F4 the GPIOxEN bits in RCC->AHB1ENR are
     * exactly bits 0..8 for ports A..I, i.e. bit == (port index). */
    RCC->AHB1ENR |= (1U << (uint32_t)port);

    uint32_t m = (uint32_t)cfg->mode & 3U;
    gpio->MODER = (gpio->MODER & ~(3U << (pin * 2))) | (m << (pin * 2));

    if (m == 2U) {                       /* alternate function */
        uint32_t idx   = (pin >= 8U) ? 1U : 0U;
        uint32_t shift = ((uint32_t)(pin & 7U)) * 4U;
        gpio->AFR[idx] = (gpio->AFR[idx] & ~(0xFU << shift))
                       | ((uint32_t)(cfg->af & 0xFU) << shift);
    }

    if (m == 1U || m == 2U) {            /* output / AF: OTYPER + OSPEEDR */
        if (cfg->otype & 1U) gpio->OTYPER |=  (1U << pin);
        else                 gpio->OTYPER &= ~(1U << pin);
        gpio->OSPEEDR = (gpio->OSPEEDR & ~(3U << (pin * 2)))
                      | ((uint32_t)(cfg->speed & 3U) << (pin * 2));
    }

    gpio->PUPDR = (gpio->PUPDR & ~(3U << (pin * 2)))
                | ((uint32_t)(cfg->pupd & 3U) << (pin * 2));
}
