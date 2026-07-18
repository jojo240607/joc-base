#ifndef UART_HAL_H
#define UART_HAL_H

#include <stdint.h>

/*
 * Hardware Abstraction Layer — USART (STM32 implementation).
 *
 * The driver layer only handles the OPAQUE `uart_hal_handle_t *`; it never sees
 * USART_TypeDef or any register. The concrete struct is private to uart_hal.c.
 * Porting to another chip = rewrite this HAL only.
 */
typedef struct uart_hal_handle uart_hal_handle_t;

#define UART_HAL_PCLK2_HZ 84000000UL   /* USART1 is on APB2 (HCLK/2) — platform clock */

uart_hal_handle_t *uart_hal_create(void *peripheral, uint32_t baud);
void uart_hal_destroy(uart_hal_handle_t *h);

void uart_hal_init(uart_hal_handle_t *h);
void uart_hal_deinit(uart_hal_handle_t *h);
void uart_hal_set_baudrate(uart_hal_handle_t *h, uint32_t baud);
void uart_hal_putc(uart_hal_handle_t *h, char c);
char uart_hal_getc(uart_hal_handle_t *h);
uint32_t uart_hal_get_baudrate(uart_hal_handle_t *h); /* read back logical baud */
uint32_t uart_hal_get_brr(uart_hal_handle_t *h);   /* read back BRR for self-test */
uint32_t uart_hal_get_cr1(uart_hal_handle_t *h);   /* read back CR1 for self-test */

#endif /* UART_HAL_H */
