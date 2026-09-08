#include "uart_hal.h"
#include "stm32f1xx.h"
#include <stdlib.h>

/* OPAQUE handle — the only USART state the HAL keeps. Hidden from the driver. */
struct uart_hal_handle {
    USART_TypeDef *usart;
    uint32_t baudrate;
};

uart_hal_handle_t *uart_hal_create(void *peripheral, uint32_t baud)
{
    uart_hal_handle_t *h = (uart_hal_handle_t *)malloc(sizeof(uart_hal_handle_t));
    if (!h) return NULL;
    h->usart    = (USART_TypeDef *)peripheral;
    h->baudrate = baud;
    return h;
}

void uart_hal_destroy(uart_hal_handle_t *h)
{
    free(h);
}

void uart_hal_init(uart_hal_handle_t *h)
{
    if (!h) return;
    USART_TypeDef *usart = h->usart;

    /* Enable USART1 clock on APB2 */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* BRR = PCLK2 / (16 * baud)
     * For 72 MHz / 115200: 72000000 / 1843200 = 39.0625
     * DIV_Mantissa = 39, DIV_Fraction = 0.0625 * 16 = 1 */
    usart->BRR = (uint32_t)(UART_HAL_PCLK2_HZ / h->baudrate);
    usart->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart_hal_deinit(uart_hal_handle_t *h)
{
    if (!h) return;
    h->usart->CR1 &= ~(USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
}

void uart_hal_set_baudrate(uart_hal_handle_t *h, uint32_t baud)
{
    if (!h) return;
    h->baudrate = baud;
    h->usart->BRR = (uint32_t)(UART_HAL_PCLK2_HZ / baud);
}

void uart_hal_set_parity(uart_hal_handle_t *h, int parity)
{
    if (!h) return;
    uint32_t cr1 = h->usart->CR1;
    cr1 &= ~(USART_CR1_PCE | USART_CR1_PS);
    if (parity == 1)      cr1 |= USART_CR1_PCE | USART_CR1_PS; /* odd  */
    else if (parity == 2) cr1 |= USART_CR1_PCE;               /* even */
    h->usart->CR1 = cr1;
}

void uart_hal_set_stopbits(uart_hal_handle_t *h, int stop)
{
    if (!h) return;
    uint32_t cr2 = h->usart->CR2;
    cr2 &= ~(USART_CR2_STOP_1 | USART_CR2_STOP_0);
    if (stop == 2) cr2 |= USART_CR2_STOP_1;
    h->usart->CR2 = cr2;
}

#define UART_HAL_WAIT_BUDGET 100000u

void uart_hal_putc(uart_hal_handle_t *h, char c)
{
    if (!h) return;
    uint32_t budget = UART_HAL_WAIT_BUDGET;
    while ((h->usart->SR & USART_SR_TXE) == 0) {
        if (--budget == 0) return;
    }
    h->usart->DR = (uint8_t)c;
}

char uart_hal_getc(uart_hal_handle_t *h)
{
    if (!h) return 0;
    uint32_t budget = UART_HAL_WAIT_BUDGET;
    while ((h->usart->SR & USART_SR_RXNE) == 0) {
        if (--budget == 0) return 0;
    }
    return (char)(h->usart->DR & 0xFFU);
}

char uart_hal_read_dr(uart_hal_handle_t *h)
{
    if (!h) return 0;
    return (char)(h->usart->DR & 0xFFU);
}

void uart_hal_enable_rx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 |= USART_CR1_RXNEIE;
}

void uart_hal_disable_rx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 &= ~USART_CR1_RXNEIE;
}

void uart_hal_enable_tx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 |= USART_CR1_TXEIE;
}

void uart_hal_disable_tx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 &= ~USART_CR1_TXEIE;
}

int uart_hal_tx_ready(uart_hal_handle_t *h)
{
    return (h && (h->usart->SR & USART_SR_TXE)) ? 1 : 0;
}

int uart_hal_rx_pending(uart_hal_handle_t *h)
{
    return (h && (h->usart->SR & USART_SR_RXNE)) ? 1 : 0;
}

int uart_hal_ore_pending(uart_hal_handle_t *h)
{
    return (h && (h->usart->SR & USART_SR_ORE)) ? 1 : 0;
}

void uart_hal_enable_idle_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 |= USART_CR1_IDLEIE;
}

void uart_hal_disable_idle_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 &= ~USART_CR1_IDLEIE;
}

int uart_hal_idle_pending(uart_hal_handle_t *h)
{
    return (h && (h->usart->SR & USART_SR_IDLE)) ? 1 : 0;
}

void uart_hal_clear_idle(uart_hal_handle_t *h)
{
    if (!h) return;
    (void)h->usart->SR;
    (void)h->usart->DR;
}

void uart_hal_clear_errors(uart_hal_handle_t *h)
{
    if (!h) return;
    (void)h->usart->SR;
    (void)h->usart->DR;
}

void uart_hal_write_dr(uart_hal_handle_t *h, char c)
{
    if (h) h->usart->DR = (uint8_t)c;
}

irq_id_t uart_hal_irq_id(uart_hal_handle_t *h)
{
    if (!h) return (irq_id_t)0;
    if (h->usart == USART1) return (irq_id_t)USART1_IRQn;
    if (h->usart == USART2) return (irq_id_t)USART2_IRQn;
    if (h->usart == USART3) return (irq_id_t)USART3_IRQn;
    return (irq_id_t)0;
}

uint32_t uart_hal_get_baudrate(uart_hal_handle_t *h)
{
    return h ? h->baudrate : 0UL;
}

uint32_t uart_hal_get_brr(uart_hal_handle_t *h)
{
    return h ? h->usart->BRR : 0UL;
}

uint32_t uart_hal_get_cr1(uart_hal_handle_t *h)
{
    return h ? h->usart->CR1 : 0UL;
}

/* No hardware invert on F1 (no RXINV/TXINV in USART_CR1) */
void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv)
{
    (void)h; (void)rx_inv; (void)tx_inv;
}

void *uart_hal_get_dr_addr(uart_hal_handle_t *h)
{
    return h ? (void *)&h->usart->DR : NULL;
}

/* DMA stubs — F1 has DMA but this minimal port skips it */
void uart_hal_enable_tx_dma(uart_hal_handle_t *h)  { if (h) h->usart->CR3 |= USART_CR3_DMAT; }
void uart_hal_disable_tx_dma(uart_hal_handle_t *h) { if (h) h->usart->CR3 &= ~USART_CR3_DMAT; }
void uart_hal_enable_rx_dma(uart_hal_handle_t *h)  { if (h) h->usart->CR3 |= USART_CR3_DMAR; }
void uart_hal_disable_rx_dma(uart_hal_handle_t *h) { if (h) h->usart->CR3 &= ~USART_CR3_DMAR; }

uint32_t uart_hal_get_sr(uart_hal_handle_t *h)
{
    return h ? h->usart->SR : 0UL;
}

uint32_t uart_hal_get_cr3(uart_hal_handle_t *h)
{
    return h ? h->usart->CR3 : 0UL;
}