#ifndef UART_STM32_H
#define UART_STM32_H

#include "serial.h"
#include "stm32f4xx.h"
#include <stdint.h>

typedef struct _uart_stm32 uart_stm32;

struct uart_stm32Fun {
    void (*destroy)(uart_stm32 *self);
    void (*init)(uart_stm32 *self);
    void (*deinit)(uart_stm32 *self);
    void (*set_baudrate)(uart_stm32 *self, uint32_t baud);
};

/* vtable extends the base serial vtable (first member is serialVtable) */
struct uart_stm32Vtable {
    struct serialVtable parent;
};

struct _uart_stm32 {
    union { serial parent; struct uart_stm32Vtable *vtable; };
    const struct uart_stm32Fun *fun;
    USART_TypeDef *instance;
    uint32_t baudrate;
};

uart_stm32 *uart_stm32_create(USART_TypeDef *usart, uint32_t baudrate);
void uart_stm32_destroy(uart_stm32 *self);
void uart_stm32_init(uart_stm32 *self);
void uart_stm32_deinit(uart_stm32 *self);
void uart_stm32_set_baudrate(uart_stm32 *self, uint32_t baud);

/* console helpers used by syscalls _write */
void uart_stm32_set_console(uart_stm32 *self);
void uart_stm32_console_putc(char c);
char uart_stm32_getc(uart_stm32 *self);   /* blocking receive */

extern const struct uart_stm32Fun uart_stm32_fun;

#endif /* UART_STM32_H */
