#include "uart_hal.h"
#include "clock_hal.h"
#include "stm32h7xx.h"
#include <stdlib.h>

/*
 * STM32H7 USART HAL — H7 uses the F7-style register set:
 *   ISR (interrupt/status) @ 0x1C, ICR (write-1-to-clear) @ 0x20,
 *   RDR @ 0x24, TDR @ 0x28. There is NO SR/DR on H7.
 * USART1 is on APB2 (120 MHz when PCLK2 = 120 MHz); USART2/3 on APB1.
 */

/* OPAQUE handle — the only USART state the HAL keeps. Hidden from the driver. */
struct uart_hal_handle {
    USART_TypeDef *usart;
    uint32_t baudrate;
};

static void uart_hal_enable_clock(USART_TypeDef *usart)
{
    if (usart == USART1)      RCC->APB2ENR  |= RCC_APB2ENR_USART1EN;
    else if (usart == USART2) RCC->APB1LENR |= RCC_APB1LENR_USART2EN;
    else if (usart == USART3) RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
}

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

    uart_hal_enable_clock(usart);

    /* BRR = PCLK / (16 * baud), OVER8=0 -> BRR = PCLK / baud.
     * For 120 MHz / 115200: DIV_Mantissa = 1041 (0x411), fraction = 0.6667 */
    usart->BRR = (uint32_t)(clock_hal_usart_hz() / h->baudrate);
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
    h->usart->BRR = (uint32_t)(clock_hal_usart_hz() / baud);
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
    while ((h->usart->ISR & USART_ISR_TXE_TXFNF) == 0) {
        if (--budget == 0) return;
    }
    h->usart->TDR = (uint8_t)c;
}

char uart_hal_getc(uart_hal_handle_t *h)
{
    if (!h) return 0;
    uint32_t budget = UART_HAL_WAIT_BUDGET;
    while ((h->usart->ISR & USART_ISR_RXNE_RXFNE) == 0) {
        if (--budget == 0) return 0;
    }
    return (char)(h->usart->RDR & 0xFFU);
}

char uart_hal_read_dr(uart_hal_handle_t *h)
{
    if (!h) return 0;
    return (char)(h->usart->RDR & 0xFFU);
}

void uart_hal_enable_rx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
}

void uart_hal_disable_rx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 &= ~USART_CR1_RXNEIE_RXFNEIE;
}

void uart_hal_enable_tx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 |= USART_CR1_TXEIE_TXFNFIE;
}

void uart_hal_disable_tx_irq(uart_hal_handle_t *h)
{
    if (h) h->usart->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
}

int uart_hal_tx_ready(uart_hal_handle_t *h)
{
    return (h && (h->usart->ISR & USART_ISR_TXE_TXFNF)) ? 1 : 0;
}

int uart_hal_rx_pending(uart_hal_handle_t *h)
{
    return (h && (h->usart->ISR & USART_ISR_RXNE_RXFNE)) ? 1 : 0;
}

int uart_hal_ore_pending(uart_hal_handle_t *h)
{
    return (h && (h->usart->ISR & USART_ISR_ORE)) ? 1 : 0;
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
    return (h && (h->usart->ISR & USART_ISR_IDLE)) ? 1 : 0;
}

void uart_hal_clear_idle(uart_hal_handle_t *h)
{
    if (!h) return;
    h->usart->ICR |= USART_ICR_IDLECF;   /* W1C on H7 (no read-SR-read-DR trick) */
}

void uart_hal_clear_errors(uart_hal_handle_t *h)
{
    if (!h) return;
    h->usart->ICR |= USART_ICR_ORECF | USART_ICR_NECF | USART_ICR_FECF | USART_ICR_PECF;
}

void uart_hal_write_dr(uart_hal_handle_t *h, char c)
{
    if (h) h->usart->TDR = (uint8_t)c;
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

/* H7 USART_CR1 has RXINV (bit 13) / TXINV (bit 14) — support them */
void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv)
{
    if (!h) return;
    uint32_t cr1 = h->usart->CR1;
    if (rx_inv) cr1 |= (1U << 13U);
    else        cr1 &= ~(1U << 13U);
    if (tx_inv) cr1 |= (1U << 14U);
    else        cr1 &= ~(1U << 14U);
    h->usart->CR1 = cr1;
}

void *uart_hal_get_dr_addr(uart_hal_handle_t *h)
{
    return h ? (void *)&h->usart->TDR : NULL;
}

/* DMA stubs — milestone 1 uses polling TX + IRQ RX only (DMA in milestone 2) */
void uart_hal_enable_tx_dma(uart_hal_handle_t *h)  { if (h) h->usart->CR3 |= USART_CR3_DMAT; }
void uart_hal_disable_tx_dma(uart_hal_handle_t *h) { if (h) h->usart->CR3 &= ~USART_CR3_DMAT; }
void uart_hal_enable_rx_dma(uart_hal_handle_t *h)  { if (h) h->usart->CR3 |= USART_CR3_DMAR; }
void uart_hal_disable_rx_dma(uart_hal_handle_t *h) { if (h) h->usart->CR3 &= ~USART_CR3_DMAR; }

uint32_t uart_hal_get_sr(uart_hal_handle_t *h)
{
    return h ? h->usart->ISR : 0UL;   /* H7: status lives in ISR */
}

uint32_t uart_hal_get_cr3(uart_hal_handle_t *h)
{
    return h ? h->usart->CR3 : 0UL;
}
