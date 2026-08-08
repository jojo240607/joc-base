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

    /* The TX/RX pins are claimed AND configured by the driver through the
     * pinmux (drv/pinmux.c -> hal/stm32/pinmux_hal.c) at open() time, so we
     * must NOT touch GPIO registers here — doing so would bypass the conflict
     * arbitrator. Only the USART peripheral itself is set up below. */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

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

/* Line-protocol parameters beyond baud (SBUS needs 100000/8E2). These bits
 * live in CR1 (parity) and CR2 (stop bits) and can be changed live while UE=1
 * per RM0090. parity: 0=none, 1=odd, 2=even. stop: 1 or 2 stop bits. */
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
    cr2 &= ~(USART_CR2_STOP_1 | USART_CR2_STOP_0); /* clear STOP[13:12] */
    if (stop == 2) cr2 |= (USART_CR2_STOP_1);      /* 0b10 = 2 stop bits */
    /* stop==1 (or any other) leaves 0b00 = 1 stop bit */
    h->usart->CR2 = cr2;
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

/* Read the data register WITHOUT blocking. Used by the receive ISR: reading DR
 * clears the RXNE flag, so the interrupt will not re-fire. */
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

/* IDLE-line interrupt: fires after the bus is silent for >1 byte time, i.e. at
 * the END of a variable-length frame. Used with a (circular) RX DMA so the ISR
 * can compute how many bytes were received (buffer_size - NDTR) and re-arm. */
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
/* Clear the IDLE flag via the mandatory read-SR-then-read-DR sequence. Without
 * this the flag stays set and the ISR re-fires forever. */
void uart_hal_clear_idle(uart_hal_handle_t *h)
{
    if (!h) return;
    (void)h->usart->SR;
    (void)h->usart->DR;
}

/* Clear latched receive errors (Overrun / Framing / Noise). On STM32 an Overrun
 * FREEZES the receiver until it is cleared (read SR then DR), so any path that
 * leaves the RX line unattended (e.g. STREAM_MODE_DMA bulk TX, or a mode switch
 * with DMAR/RXNE both off while bytes keep arriving) must clear it before
 * re-arming reception, otherwise the next DMA/RX read silently starves. */
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

/* Return the chip interrupt id for this USART so the driver can register its
 * ISR through the platform-independent irq framework without naming a
 * Cortex-M / STM32 interrupt directly. */
irq_id_t uart_hal_irq_id(uart_hal_handle_t *h)
{
    if (!h) return (irq_id_t)0;
    if (h->usart == USART1) return (irq_id_t)USART1_IRQn;
    if (h->usart == USART2) return (irq_id_t)USART2_IRQn;
    if (h->usart == USART3) return (irq_id_t)USART3_IRQn;
    if (h->usart == UART4)  return (irq_id_t)UART4_IRQn;
    if (h->usart == UART5)  return (irq_id_t)UART5_IRQn;
    if (h->usart == USART6) return (irq_id_t)USART6_IRQn;
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

/* Logic-level inversion for SBUS / inverted peripherals.
 *
 * NOTE: STM32F407's USART has NO hardware invert bit (RXINV/TXINV exist only on
 * F3/L0/F7/H7). On F4 the SBUS inverted line MUST be handled by an external
 * inverter (NPN + pull-up, or a vendor USB-UART adapter with invert jumper).
 * This function is therefore a no-op on F4 — it keeps the driver ABI stable so
 * the Rust SBUS layer's SET_INVERTED call is harmless, but does NOT touch any
 * register (there is none). If you port to an F7/H7, implement the CR1 bits. */
void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv)
{
    (void)h; (void)rx_inv; (void)tx_inv;
    /* F4: no hardware invert. SBUS inversion is external. */
}

void *uart_hal_get_dr_addr(uart_hal_handle_t *h)
{
    return h ? (void *)&h->usart->DR : NULL;
}

void uart_hal_enable_tx_dma(uart_hal_handle_t *h)
{
    if (h) h->usart->CR3 |= USART_CR3_DMAT;
}
void uart_hal_disable_tx_dma(uart_hal_handle_t *h)
{
    if (h) h->usart->CR3 &= ~USART_CR3_DMAT;
}
void uart_hal_enable_rx_dma(uart_hal_handle_t *h)
{
    if (h) h->usart->CR3 |= USART_CR3_DMAR;
}
void uart_hal_disable_rx_dma(uart_hal_handle_t *h)
{
    if (h) h->usart->CR3 &= ~USART_CR3_DMAR;
}

uint32_t uart_hal_get_sr(uart_hal_handle_t *h)
{
    return h ? h->usart->SR : 0UL;
}
uint32_t uart_hal_get_cr3(uart_hal_handle_t *h)
{
    return h ? h->usart->CR3 : 0UL;
}
