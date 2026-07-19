#include "uart.h"
#include "devmgr/device_manager.h"   /* resolve the pinmux arbiter by name */
#include "drv/pinmux.h"               /* request + program pins through pinmux */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                     /* printf for conflict diagnostics */

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

/* RX ring-buffer + ISR helpers (defined below; declared here so uart_getc can
 * call uart_rx_getc before its definition). */
static void uart_rx_putc(uart *self, char c);
static char uart_rx_getc(uart *self);
static void uart_isr(void *ctx);

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
    self->tx_signal = c->tx_signal;          /* cache names for pinmux claim at open() */
    self->rx_signal = c->rx_signal;
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
    /* drain the RX ring buffer (filled by the receive ISR) */
    return self ? uart_rx_getc(self) : 0;
}

/* --- RX ring buffer + interrupt ISR (platform-independent irq framework) --- */

/* Push one received byte into the ring buffer (called from interrupt context). */
static void uart_rx_putc(uart *self, char c)
{
    uint16_t next = (uint16_t)((self->rx_head + 1U) % UART_RX_BUF_SIZE);
    if (next != self->rx_tail) {            /* drop on overflow */
        self->rx_buf[self->rx_head] = c;
        self->rx_head = next;
    }
}

/* Pop one byte, blocking until the ISR delivers one (thread context). */
static char uart_rx_getc(uart *self)
{
    while (self->rx_head == self->rx_tail) { /* wait for the ISR to fill */ }
    char c = self->rx_buf[self->rx_tail];
    self->rx_tail = (uint16_t)((self->rx_tail + 1U) % UART_RX_BUF_SIZE);
    return c;
}

/* The receive ISR callback. Registered with the framework via irq_register()
 * (see uart_dev_open); `ctx` is the uart instance. Reading DR clears RXNE. */
static void uart_isr(void *ctx)
{
    uart *u = (uart *)ctx;
    uart_rx_putc(u, uart_hal_read_dr(u->hal));
}

/* --- unified device-interface virtual implementations --- */

static int uart_dev_open(device *self)
{
    uart *u = (uart *)self;

    /* Claim + program the TX/RX pins through the pinmux BEFORE touching any
     * GPIO register. The board supplied a SIGNAL NAME (e.g. "USART1_TX_PA9");
     * pinmux_hal_resolve() turns it into the exact (port, pin, af). Because the
     * AF database has NO duplicate names (suffixes guarantee uniqueness), the
     * name maps to exactly one pad — no "first match wins" ambiguity. If a pin
     * is already owned by another device the request fails and we refuse to
     * configure it (and roll back any pin already claimed) — that is the
     * arbitrator. */
    pinmux *pm = (pinmux *)device_manager_get("pinmux");
    if (pm) {
        pinmux_port_t port; uint8_t pin, af;
        pinmux_pin_cfg_t cfg = { .mode = 2, .otype = 0, .speed = 3, .pupd = 0 };

        /* TX */
        if (!pinmux_hal_resolve(u->tx_signal, &port, &pin, &af)) {
            printf("[uart] %s: unknown TX signal \"%s\"\r\n", u->parent.name, u->tx_signal);
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.name) != 0) {
            printf("[uart] %s: TX pin P%c%d CONFLICT — refused\r\n",
                   u->parent.name, 'A' + port, pin);
            return -2;                       /* conflict: do NOT configure */
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);

        /* RX */
        if (!pinmux_hal_resolve(u->rx_signal, &port, &pin, &af)) {
            printf("[uart] %s: unknown RX signal \"%s\"\r\n", u->parent.name, u->rx_signal);
            pm->fun->release_owner(pm, u->parent.name);   /* roll back TX */
            return -3;
        }
        if (pm->fun->request(pm, port, pin, af, u->parent.name) != 0) {
            printf("[uart] %s: RX pin P%c%d CONFLICT — refused\r\n",
                   u->parent.name, 'A' + port, pin);
            pm->fun->release_owner(pm, u->parent.name);   /* roll back TX */
            return -2;
        }
        cfg.af = af;
        pm->fun->config(pm, port, pin, &cfg);
    }

    uart_hal_init(u->hal);

    /* Wire the RX interrupt through the PLATFORM-INDEPENDENT irq framework.
     * The driver registers a callback + its own context; the HAL supplies the
     * chip IRQ number (uart_hal_irq_id), so the driver never names a Cortex-M /
     * STM32 interrupt directly. The ISR (uart_isr) reads DR and fills the ring
     * buffer; read()/getc() then drain it. */
    irq_id_t rx_irq = uart_hal_irq_id(u->hal);
    irq_register(rx_irq, uart_isr, u);
    irq_set_priority(rx_irq, 0);
    uart_hal_enable_rx_irq(u->hal);
    irq_enable(rx_irq);
    return 0;
}

static int uart_dev_close(device *self)
{
    uart *u = (uart *)self;
    irq_id_t rx_irq = uart_hal_irq_id(u->hal);
    irq_disable(rx_irq);                 /* stop the ISR first */
    uart_hal_disable_rx_irq(u->hal);
    irq_register(rx_irq, NULL, NULL);   /* uninstall the callback */
    uart_hal_deinit(u->hal);
    return 0;
}

static int uart_dev_read(device *self, void *buf, size_t len)
{
    uart *u = (uart *)self;
    if (len < 1 || !buf) return -1;
    *(char *)buf = uart_rx_getc(u);      /* drain the ISR-fed ring buffer */
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
