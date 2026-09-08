#ifndef UART_HAL_H
#define UART_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/*
 * Hardware Abstraction Layer — USART (STM32F1 implementation).
 *
 * The driver layer only handles the OPAQUE `uart_hal_handle_t *`; it never sees
 * USART_TypeDef or any register. The concrete struct is private to uart_hal.c.
 * Porting to another chip = rewrite this HAL only.
 *
 * STM32F1 USART1 is on APB2 (72 MHz when HCLK = 72 MHz).
 */
typedef struct uart_hal_handle uart_hal_handle_t;

#define UART_HAL_PCLK2_HZ 72000000UL    /* USART1 is on APB2 — platform clock */

uart_hal_handle_t *uart_hal_create(void *peripheral, uint32_t baud);
void uart_hal_destroy(uart_hal_handle_t *h);

void uart_hal_init(uart_hal_handle_t *h);
void uart_hal_deinit(uart_hal_handle_t *h);
void uart_hal_set_baudrate(uart_hal_handle_t *h, uint32_t baud);
void uart_hal_set_parity(uart_hal_handle_t *h, int parity);   /* 0=none,1=odd,2=even */
void uart_hal_set_stopbits(uart_hal_handle_t *h, int stop);   /* 1 or 2 stop bits */
void uart_hal_putc(uart_hal_handle_t *h, char c);
char uart_hal_getc(uart_hal_handle_t *h);
uint32_t uart_hal_get_baudrate(uart_hal_handle_t *h); /* read back logical baud */

/* --- interrupt support --- */
char uart_hal_read_dr(uart_hal_handle_t *h);     /* read DR (clears RXNE) */
void uart_hal_enable_rx_irq(uart_hal_handle_t *h);  /* set USART_CR1_RXNEIE */
void uart_hal_disable_rx_irq(uart_hal_handle_t *h); /* clear USART_CR1_RXNEIE */
irq_id_t uart_hal_irq_id(uart_hal_handle_t *h);    /* chip IRQn for this USART */
void uart_hal_enable_tx_irq(uart_hal_handle_t *h);  /* set USART_CR1_TXEIE */
void uart_hal_disable_tx_irq(uart_hal_handle_t *h); /* clear USART_CR1_TXEIE */
int  uart_hal_tx_ready(uart_hal_handle_t *h);       /* (SR & USART_SR_TXE) != 0 */
int  uart_hal_rx_pending(uart_hal_handle_t *h);     /* (SR & USART_SR_RXNE) != 0 */
int  uart_hal_ore_pending(uart_hal_handle_t *h);    /* (SR & USART_SR_ORE) != 0 */
void uart_hal_write_dr(uart_hal_handle_t *h, char c);/* write DR (triggers TX) */

/* --- IDLE-line interrupt --- */
void uart_hal_enable_idle_irq(uart_hal_handle_t *h);   /* set USART_CR1_IDLEIE */
void uart_hal_disable_idle_irq(uart_hal_handle_t *h);  /* clear USART_CR1_IDLEIE */
int  uart_hal_idle_pending(uart_hal_handle_t *h);      /* (SR & USART_SR_IDLE) != 0 */
void uart_hal_clear_idle(uart_hal_handle_t *h);        /* read SR then DR (clears IDLE) */
void uart_hal_clear_errors(uart_hal_handle_t *h);      /* read SR then DR (clears ORE/FE/NE) */
uint32_t uart_hal_get_brr(uart_hal_handle_t *h);   /* read back BRR for self-test */
uint32_t uart_hal_get_cr1(uart_hal_handle_t *h);   /* read back CR1 for self-test */

/* --- logic-level inversion (no-op on F1, no RXINV/TXINV bits) --- */
void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv);

/* --- DMA support (stub on F1 for minimal port, no DMA yet) --- */
void    *uart_hal_get_dr_addr(uart_hal_handle_t *h);
void uart_hal_enable_tx_dma(uart_hal_handle_t *h);
void uart_hal_disable_tx_dma(uart_hal_handle_t *h);
void uart_hal_enable_rx_dma(uart_hal_handle_t *h);
void uart_hal_disable_rx_dma(uart_hal_handle_t *h);

/* readback getters for debugging */
uint32_t uart_hal_get_sr(uart_hal_handle_t *h);
uint32_t uart_hal_get_cr3(uart_hal_handle_t *h);

#endif /* UART_HAL_H */