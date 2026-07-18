#include "uart_hal.h"
#include "stm32f4xx.h"
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
    h->usart   = (USART_TypeDef *)peripheral;
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

    /* Enable GPIOA (AHB1) and USART1 (APB2) clocks */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA9 = TX, PA10 = RX, Alternate Function 7 (USART1) */
    GPIOA->MODER   = (GPIOA->MODER   & ~((3U << (9 * 2)) | (3U << (10 * 2))))
                   |  ((2U << (9 * 2)) | (2U << (10 * 2)));
    GPIOA->OTYPER &= ~((1U << 9) | (1U << 10));                          /* push-pull */
    GPIOA->OSPEEDR |= ((3U << (9 * 2)) | (3U << (10 * 2)));              /* high speed */
    GPIOA->PUPDR  &= ~((3U << (9 * 2)) | (3U << (10 * 2)));              /* no pull */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4))))
                  |  ((7U << ((9 - 8) * 4)) | (7U << ((10 - 8) * 4)));   /* AF7 */

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

void uart_hal_putc(uart_hal_handle_t *h, char c)
{
    if (!h) return;
    while ((h->usart->SR & USART_SR_TXE) == 0) { }
    h->usart->DR = (uint8_t)c;
}

char uart_hal_getc(uart_hal_handle_t *h)
{
    if (!h) return 0;
    while ((h->usart->SR & USART_SR_RXNE) == 0) { }
    return (char)(h->usart->DR & 0xFFU);
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
