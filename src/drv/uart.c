#include "uart.h"
#include <stdlib.h>
#include <string.h>

static uart *g_console = NULL;

/* virtual implementations dispatched through the unified device vtable */
static int uart_dev_open(device *self);
static int uart_dev_close(device *self);
static int uart_dev_read(device *self, void *buf, size_t len);
static int uart_dev_write(device *self, const void *buf, size_t len);
static int uart_dev_ioctl(device *self, int cmd, void *arg);

/* public methods — `static`, reachable ONLY through self->fun-> */
static void uart_set_baudrate(uart *self, uint32_t baud);
static char uart_getc(uart *self);

const struct uartFun uart_fun = {
    .destroy      = uart_destroy,
    .init         = uart_init,
    .deinit       = uart_deinit,
    .set_baudrate = uart_set_baudrate,
    .getc         = uart_getc,
};

uart *uart_create(uart_hal_handle_t *hal, const char *name)
{
    uart *self = (uart *)malloc(sizeof(uart));
    if (!self) return NULL;
    memset(self, 0, sizeof(uart));
    self->hal = hal;
    self->parent.type = DEVICE_TYPE_UART;    /* driver sets its own class */
    self->parent.name = name;                /* driver sets its own name */
    uart_init(self);
    return self;
}

void uart_destroy(uart *self)
{
    if (!self) return;
    uart_deinit(self);
    free(self);
}

void uart_init(uart *self)
{
    if (!self) return;
    device_init(&self->parent);
    self->fun = &uart_fun;
    self->parent.vtable->open  = uart_dev_open;
    self->parent.vtable->close = uart_dev_close;
    self->parent.vtable->read  = uart_dev_read;
    self->parent.vtable->write = uart_dev_write;
    self->parent.vtable->ioctl = uart_dev_ioctl;
    self->parent.vtable->open((device *)self);   /* bring up USART now */
}

void uart_deinit(uart *self)
{
    if (!self) return;
    device_deinit(&self->parent);
}

static void uart_set_baudrate(uart *self, uint32_t baud)
{
    if (!self) return;
    uart_hal_set_baudrate(self->hal, baud);
}

void uart_set_console(uart *self)
{
    g_console = self;
}

void uart_console_putc(char c)
{
    if (g_console)
        uart_hal_putc(g_console->hal, c);
}

static char uart_getc(uart *self)
{
    return self ? uart_hal_getc(self->hal) : 0;
}

/* --- unified device-interface virtual implementations --- */

static int uart_dev_open(device *self)
{
    uart_hal_init(((uart *)self)->hal);
    return 0;
}

static int uart_dev_close(device *self)
{
    uart_hal_deinit(((uart *)self)->hal);
    return 0;
}

static int uart_dev_read(device *self, void *buf, size_t len)
{
    uart *u = (uart *)self;
    if (len < 1 || !buf) return -1;
    *(char *)buf = uart_hal_getc(u->hal);
    return 1;
}

static int uart_dev_write(device *self, const void *buf, size_t len)
{
    uart *u = (uart *)self;
    const char *s = (const char *)buf;
    if (!buf) return -1;
    for (size_t i = 0; i < len; i++)
        uart_hal_putc(u->hal, s[i]);
    return (int)len;
}

static int uart_dev_ioctl(device *self, int cmd, void *arg)
{
    uart *u = (uart *)self;
    switch (cmd) {
    case UART_IOCTL_SET_BAUDRATE:
        if (!arg) return -1;
        uart_set_baudrate(u, *(const uint32_t *)arg);
        return 0;
    case UART_IOCTL_GET_BAUDRATE:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_baudrate(u->hal);
        return 0;
    case UART_IOCTL_GET_BRR:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_brr(u->hal);
        return 0;
    case UART_IOCTL_GET_CR1:
        if (!arg) return -1;
        *(uint32_t *)arg = uart_hal_get_cr1(u->hal);
        return 0;
    default:
        return -1;
    }
}
