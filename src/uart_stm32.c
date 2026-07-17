#include "uart_stm32.h"
#include <stdlib.h>
#include <string.h>

/* PCLK2 = HCLK(168 MHz) / APB2 prescaler(2) = 84 MHz (USART1 is on APB2) */
#define UART_PCLK2_HZ 84000000UL

static uart_stm32 *g_console = NULL;

static void uart_stm32_hw_init(uart_stm32 *self);
static void uart_stm32_hw_deinit(uart_stm32 *self);
static void uart_stm32_vputc(serial *self, char c);
static void uart_stm32_vputs(serial *self, const char *s);
static char uart_stm32_vgetc(serial *self);

const struct uart_stm32Fun uart_stm32_fun = {
    .destroy = uart_stm32_destroy,
    .init = uart_stm32_init,
    .deinit = uart_stm32_deinit,
    .set_baudrate = uart_stm32_set_baudrate,
};

uart_stm32 *uart_stm32_create(USART_TypeDef *usart, uint32_t baudrate)
{
    uart_stm32 *self = (uart_stm32 *)malloc(sizeof(uart_stm32));
    if (!self) return NULL;
    memset(self, 0, sizeof(uart_stm32));
    self->instance = usart;
    self->baudrate = baudrate;
    uart_stm32_init(self);
    return self;
}

void uart_stm32_destroy(uart_stm32 *self)
{
    if (!self) return;
    uart_stm32_deinit(self);
    free(self);
}

void uart_stm32_init(uart_stm32 *self)
{
    if (!self) return;

    /* OOC vtable allocation (extended vtable, layout-compatible with serialVtable) */
    if (!self->vtable) {
        self->vtable = (struct uart_stm32Vtable *)malloc(sizeof(struct uart_stm32Vtable));
        if (self->vtable) memset(self->vtable, 0, sizeof(struct uart_stm32Vtable));
    }
    /* serial_init sees vtable already set (union alias) and only sets fun + overrides */
    serial_init(&self->parent);
    self->fun = &uart_stm32_fun;

    /* override base virtual methods with the USART implementation */
    self->parent.vtable->putc = uart_stm32_vputc;
    self->parent.vtable->puts = uart_stm32_vputs;
    self->parent.vtable->getc = uart_stm32_vgetc;

    uart_stm32_hw_init(self);
}

void uart_stm32_deinit(uart_stm32 *self)
{
    if (!self) return;
    uart_stm32_hw_deinit(self);
    serial_deinit(&self->parent);   /* frees the shared vtable */
}

void uart_stm32_set_baudrate(uart_stm32 *self, uint32_t baud)
{
    if (!self) return;
    self->baudrate = baud;
    self->instance->BRR = (uint32_t)(UART_PCLK2_HZ / baud);
}

void uart_stm32_set_console(uart_stm32 *self)
{
    g_console = self;
}

void uart_stm32_console_putc(char c)
{
    if (g_console)
        uart_stm32_vputc((serial *)g_console, c);
}

static void uart_stm32_vputc(serial *self, char c)
{
    uart_stm32 *u = (uart_stm32 *)self;
    while ((u->instance->SR & USART_SR_TXE) == 0) { }
    u->instance->DR = (uint8_t)c;
}

static void uart_stm32_vputs(serial *self, const char *s)
{
    if (!s) return;
    while (*s)
        uart_stm32_vputc(self, *s++);
}

static char uart_stm32_vgetc(serial *self)
{
    uart_stm32 *u = (uart_stm32 *)self;
    while ((u->instance->SR & USART_SR_RXNE) == 0) { }
    return (char)(u->instance->DR & 0xFFU);
}

char uart_stm32_getc(uart_stm32 *self)
{
    return self ? uart_stm32_vgetc((serial *)self) : 0;
}

static void uart_stm32_hw_init(uart_stm32 *self)
{
    USART_TypeDef *usart = self->instance;

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

    usart->BRR = (uint32_t)(UART_PCLK2_HZ / self->baudrate);
    usart->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void uart_stm32_hw_deinit(uart_stm32 *self)
{
    self->instance->CR1 &= ~(USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
}
