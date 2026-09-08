#include "pinmux_hal.h"
#include "stm32f1xx.h"
#include <stdlib.h>

/*
 * STM32F1 GPIO register programming for the pinmux.
 *
 * F1 uses CRL (pins 0-7) and CRH (pins 8-15) instead of MODER/OTYPER/OSPEEDR.
 * Each 4-bit nibble: MODEx[1:0] at bits [4n+3:4n+2], CNFx[1:0] at [4n+1:4n].
 *
 * MODE bits:
 *   00 = input
 *   01 = output 10 MHz
 *   10 = output 2 MHz
 *   11 = output 50 MHz
 *
 * CNF bits (input mode):
 *   00 = analog
 *   01 = floating input (default)
 *   10 = input with pull-up/pull-down
 *
 * CNF bits (output mode):
 *   00 = push-pull
 *   01 = open-drain
 *   10 = alternate function push-pull
 *   11 = alternate function open-drain
 */
#define GPIOA_BASE_ADDR 0x40010800UL
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

/*
 * Convert the logical cfg to F1 CRL/CRH nibble values.
 *
 * F1 CRL/CRH use (CNF, MODE) pairs within a 4-bit nibble:
 *   bits [1:0] = MODEy (output speed, or 00 for input)
 *   bits [3:2] = CNFy  (config)
 *
 * Mapping from (mode, otype, speed):
 *   input  (mode=0): MODE=00, CNF based on pupd
 *   output (mode=1): MODE=speed, CNF based on otype
 *   alter. (mode=2): MODE=speed, CNF=10 (push-pull) or 11 (open-drain)
 *   analog (mode=3): MODE=00, CNF=00 (analog)
 */
void pinmux_hal_config(pinmux_port_t port, uint8_t pin,
                       const pinmux_pin_cfg_t *cfg)
{
    if (!cfg) return;
    GPIO_TypeDef *gpio = port_base(port);
    if (!gpio || pin >= 16U) return;

    /* Enable GPIO port clock on APB2 (RCC_APB2ENR bits 2..8 for A..G) */
    RCC->APB2ENR |= (1U << ((uint32_t)port + 2U));

    uint32_t mode_bits;
    uint32_t cnf_bits;

    switch (cfg->mode & 0x03U) {
    default: /* 0 = input */
        mode_bits = 0U;
        if (cfg->pupd == 1U || cfg->pupd == 2U) {
            cnf_bits = 2U;               /* pull-up/down (ODR controls direction) */
            /* Set/reset ODR bit for pull-up/pull-down */
            if (cfg->pupd == 1U)
                gpio->ODR |= (1U << pin);   /* pull-up */
            else
                gpio->ODR &= ~(1U << pin);  /* pull-down */
        } else {
            cnf_bits = 1U;               /* floating input */
        }
        break;

    case 1: /* output */
        switch (cfg->speed & 0x03U) {
        case 0:  mode_bits = 1U; break;  /* 10 MHz */
        case 1:  mode_bits = 2U; break;  /* 2 MHz */
        default: mode_bits = 3U; break;  /* 50 MHz */
        }
        cnf_bits = (cfg->otype & 1U) ? 1U : 0U; /* open-drain or push-pull */
        break;

    case 2: /* alternate function */
        switch (cfg->speed & 0x03U) {
        case 0:  mode_bits = 1U; break;  /* 10 MHz */
        case 1:  mode_bits = 2U; break;  /* 2 MHz */
        default: mode_bits = 3U; break;  /* 50 MHz */
        }
        cnf_bits = (cfg->otype & 1U) ? 3U : 2U; /* alt open-drain / push-pull */
        break;

    case 3: /* analog */
        mode_bits = 0U;
        cnf_bits  = 0U;
        break;
    }

    uint32_t nibble = (cnf_bits << 2U) | mode_bits;
    uint32_t shift;

    if (pin < 8U) {
        shift = pin * 4U;
        gpio->CRL = (gpio->CRL & ~(0xFU << shift)) | (nibble << shift);
    } else {
        shift = (pin - 8U) * 4U;
        gpio->CRH = (gpio->CRH & ~(0xFU << shift)) | (nibble << shift);
    }
}