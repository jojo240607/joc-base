#ifndef UART_HAL_H
#define UART_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

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

/* --- interrupt support (used by the driver via the platform-independent
 *     irq framework: it registers uart_hal_irq_id() with irq_register) --- */
char uart_hal_read_dr(uart_hal_handle_t *h);     /* read DR (clears RXNE) */
void uart_hal_enable_rx_irq(uart_hal_handle_t *h);  /* set USART_CR1_RXNEIE */
void uart_hal_disable_rx_irq(uart_hal_handle_t *h); /* clear USART_CR1_RXNEIE */
irq_id_t uart_hal_irq_id(uart_hal_handle_t *h);    /* chip IRQn for this USART */
/* --- TX interrupt support (drive blocking writes / async submit from the TXE ISR) --- */
void uart_hal_enable_tx_irq(uart_hal_handle_t *h);  /* set USART_CR1_TXEIE */
void uart_hal_disable_tx_irq(uart_hal_handle_t *h); /* clear USART_CR1_TXEIE */
int  uart_hal_tx_ready(uart_hal_handle_t *h);       /* (SR & USART_SR_TXE) != 0 */
int  uart_hal_rx_pending(uart_hal_handle_t *h);     /* (SR & USART_SR_RXNE) != 0 */
void uart_hal_write_dr(uart_hal_handle_t *h, char c);/* write DR (triggers TX) */
uint32_t uart_hal_get_brr(uart_hal_handle_t *h);   /* read back BRR for self-test */
uint32_t uart_hal_get_cr1(uart_hal_handle_t *h);   /* read back CR1 for self-test */

/* --- DMA support (used by the driver's STREAM_MODE_DMA engine) ---
 * The driver acquires a DMA stream (via the dma device + dma_hal_route) and
 * programs PAR = uart_hal_get_dr_addr(). These bits gate the USART's request
 * to the DMA controller: DMAT lets the DMA push each byte into DR (TX), DMAR
 * lets the DMA read each received byte out of DR (RX). */
void    *uart_hal_get_dr_addr(uart_hal_handle_t *h);   /* &USARTx->DR (for DMA PAR) */
void uart_hal_enable_tx_dma(uart_hal_handle_t *h);     /* CR3.DMAT = 1 */
void uart_hal_disable_tx_dma(uart_hal_handle_t *h);    /* CR3.DMAT = 0 */
void uart_hal_enable_rx_dma(uart_hal_handle_t *h);     /* CR3.DMAR = 1 */
void uart_hal_disable_rx_dma(uart_hal_handle_t *h);    /* CR3.DMAR = 0 */

/* readback getters for DMA debugging */
uint32_t uart_hal_get_sr(uart_hal_handle_t *h);        /* USARTx->SR */
uint32_t uart_hal_get_cr3(uart_hal_handle_t *h);       /* USARTx->CR3 (DMAR/DMAT) */

#endif /* UART_HAL_H */
