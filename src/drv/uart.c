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

/* one shared vtable for the whole UART class — assigned by uart_init() */
static const struct deviceVtable uart_dev_vtable = {
    .open  = uart_dev_open,
    .close = uart_dev_close,
    .read  = uart_dev_read,
    .write = uart_dev_write,
    .ioctl = uart_dev_ioctl,
};

/* Uniform create signature for the board layer: takes ONLY the driver's own
 * config pointer and returns a device *. The board lists this fn directly as a
 * node — no per-driver build wrapper. */
device *uart_create(const void *config)
{
    const uart_config_t *c = (const uart_config_t *)config;
    uart *self = (uart *)malloc(sizeof(uart));
    if (!self) return NULL;
    memset(self, 0, sizeof(uart));
    self->hal = uart_hal_create(c->periph, c->baud);
    if (!self->hal) { free(self); return NULL; }   /* #9: HAL alloc failure */
    self->parent.type = DEVICE_TYPE_UART;    /* driver sets its own class */
    self->parent.name = c->name;             /* driver sets its own name */
    uart_init(self);
    if (c->is_console) uart_set_console(self);
    return (device *)self;
}

void uart_destroy(uart *self)
{
    if (!self) return;
    uart_deinit(self);
    uart_hal_destroy(self->hal);   /* mirror create: free the HAL handle */
    free(self);
}

void uart_init(uart *self)
{
    if (!self) return;
    self->parent.vtable = &uart_dev_vtable;   /* per-class shared vtable */
    self->fun = &uart_fun;
    /* hardware bring-up is deferred to open() (see uart_dev_open) */
}

void uart_deinit(uart *self)
{
    if (!self) return;
    /* no base vtable to free (it is per-class static const) */
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
