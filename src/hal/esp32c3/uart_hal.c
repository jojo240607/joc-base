#include "uart_hal.h"
#include "device/esp32c3.h"      /* UART0 base, NS16550 register offsets/bits */
#include "riscv.h"               /* RISCV_IRQ_EXTERNAL_BASE */
#include <stdlib.h>              /* malloc/free */

/*
 * ESP32-C3 UART HAL (Phase-1 Renode: NS16550 register layout).
 *
 * The opaque handle is a (base, baud) pair. All register access is BYTE-wide
 * (volatile uint8_t *): the 16550 packs RBR/THR, IER, FCR/IIR, LCR into one
 * 32-bit word, so 32-bit stores would clobber neighbouring registers.
 *
 * 16550 conventions used:
 *   - LCR 0x03 = 8N1 (DLAB=0); DLAB=0x80 gates DLL/DLH (baud divisor)
 *   - divisor = UART_HAL_BAUD_CLK_HZ / (16 * baud)
 *   - LSR.DR  (bit0) = RX data ready; LSR.THRE (bit5) = TX holding empty
 *   - IER.ERBFI (bit0) = RX data interrupt; IER.ETBEI (bit1) = TX empty int
 *   - reading LSR clears the sticky OE/PE/FE/BI error bits
 */

struct uart_hal_handle {
    volatile uint8_t *base;     /* UART0 base (byte-accessible registers) */
    uint32_t baud;              /* logical baud rate */
};

static uint32_t uart_divisor(uint32_t baud)
{
    if (baud == 0u) baud = 115200u;
    return UART_HAL_BAUD_CLK_HZ / 16u / baud;
}

static void uart_reg_init(uart_hal_handle_t *h)
{
    volatile uint8_t *r = h->base;
    /* 8N1, DLAB=0 */
    r[ESP32C3_UART_LCR] = ESP32C3_UART_LCR_WLEN8;
    /* FIFO on, RX trigger 1 byte, clear both FIFOs */
    r[ESP32C3_UART_FCR] = ESP32C3_UART_FCR_FIFO | ESP32C3_UART_FCR_RXCLR |
                          ESP32C3_UART_FCR_TXCLR | ESP32C3_UART_FCR_TRG1;
    /* baud divisor via DLAB (DLH first, then DLL: 16550 requires the DLH
     * write when the divisor crosses the 8-bit boundary; harmless anyway) */
    uint32_t div = uart_divisor(h->baud);
    r[ESP32C3_UART_LCR] = ESP32C3_UART_LCR_WLEN8 | ESP32C3_UART_LCR_DLAB;
    r[ESP32C3_UART_DLH] = (uint8_t)((div >> 8) & 0xFFu);
    r[ESP32C3_UART_DLL] = (uint8_t)(div & 0xFFu);
    r[ESP32C3_UART_LCR] = ESP32C3_UART_LCR_WLEN8;
    /* no interrupts until the driver asks for them */
    r[ESP32C3_UART_IER] = 0u;
    r[ESP32C3_UART_MCR] = 0u;
}

uart_hal_handle_t *uart_hal_create(void *peripheral, uint32_t baud)
{
    if (!peripheral) return NULL;
    uart_hal_handle_t *h = (uart_hal_handle_t *)malloc(sizeof(*h));
    if (!h) return NULL;
    h->base = (volatile uint8_t *)peripheral;
    h->baud = (baud == 0u) ? 115200u : baud;
    /* Program the line now so the early console (board open / panic prints)
     * can emit before the driver's open() path runs — idempotent. */
    uart_reg_init(h);
    return h;
}

void uart_hal_destroy(uart_hal_handle_t *h)
{
    free(h);
}

void uart_hal_init(uart_hal_handle_t *h)
{
    if (!h) return;
    uart_reg_init(h);       /* idempotent re-program */
}

void uart_hal_deinit(uart_hal_handle_t *h)
{
    if (!h) return;
    h->base[ESP32C3_UART_IER] = 0u;   /* silence all UART interrupts */
}

void uart_hal_set_baudrate(uart_hal_handle_t *h, uint32_t baud)
{
    if (!h || baud == 0u) return;
    h->baud = baud;
    volatile uint8_t *r = h->base;
    uint32_t div = uart_divisor(baud);
    r[ESP32C3_UART_LCR] = ESP32C3_UART_LCR_WLEN8 | ESP32C3_UART_LCR_DLAB;
    r[ESP32C3_UART_DLH] = (uint8_t)((div >> 8) & 0xFFu);
    r[ESP32C3_UART_DLL] = (uint8_t)(div & 0xFFu);
    r[ESP32C3_UART_LCR] = ESP32C3_UART_LCR_WLEN8;
}

/* Line protocol beyond baud (SBUS needs 100000/8E2). The 16550 encodes parity
 * in LCR: bit3 PEN, bit4 EPS (0=odd, 1=even); stop bits in LCR bit2. */
void uart_hal_set_parity(uart_hal_handle_t *h, int parity)
{
    if (!h) return;
    volatile uint8_t *r = h->base;
    uint8_t lcr = r[ESP32C3_UART_LCR] & (uint8_t)~0x18u;
    if (parity == 1)      lcr |= 0x08u;          /* odd  */
    else if (parity == 2) lcr |= 0x18u;          /* even */
    r[ESP32C3_UART_LCR] = lcr;
}

void uart_hal_set_stopbits(uart_hal_handle_t *h, int stop)
{
    if (!h) return;
    volatile uint8_t *r = h->base;
    uint8_t lcr = r[ESP32C3_UART_LCR] & (uint8_t)~0x04u;
    if (stop == 2) lcr |= 0x04u;
    r[ESP32C3_UART_LCR] = lcr;
}

void uart_hal_putc(uart_hal_handle_t *h, char c)
{
    if (!h) return;
    volatile uint8_t *r = h->base;
    while (!(r[ESP32C3_UART_LSR] & ESP32C3_UART_LSR_THRE)) { }
    r[ESP32C3_UART_THR] = (uint8_t)c;
}

char uart_hal_getc(uart_hal_handle_t *h)
{
    if (!h) return (char)-1;
    volatile uint8_t *r = h->base;
    while (!(r[ESP32C3_UART_LSR] & ESP32C3_UART_LSR_DR)) { }
    return (char)r[ESP32C3_UART_RBR];
}

uint32_t uart_hal_get_baudrate(uart_hal_handle_t *h)
{
    return h ? h->baud : 0UL;
}

char uart_hal_read_dr(uart_hal_handle_t *h)
{
    return (h && h->base) ? (char)h->base[ESP32C3_UART_RBR] : (char)-1;
}

void uart_hal_enable_rx_irq(uart_hal_handle_t *h)
{
    if (!h) return;
    h->base[ESP32C3_UART_IER] |= ESP32C3_UART_IER_ERBFI;
}

void uart_hal_disable_rx_irq(uart_hal_handle_t *h)
{
    if (!h) return;
    h->base[ESP32C3_UART_IER] &= (uint8_t)~ESP32C3_UART_IER_ERBFI;
}

irq_id_t uart_hal_irq_id(uart_hal_handle_t *h)
{
    (void)h;
    /* PLIC source 13 (UART0) -> framework id 16 + 13 = 29 */
    return (irq_id_t)(RISCV_IRQ_EXTERNAL_BASE + ESP32C3_IRQ_UART0);
}

void uart_hal_enable_tx_irq(uart_hal_handle_t *h)
{
    if (!h) return;
    h->base[ESP32C3_UART_IER] |= ESP32C3_UART_IER_ETBEI;
}

void uart_hal_disable_tx_irq(uart_hal_handle_t *h)
{
    if (!h) return;
    h->base[ESP32C3_UART_IER] &= (uint8_t)~ESP32C3_UART_IER_ETBEI;
}

int uart_hal_tx_ready(uart_hal_handle_t *h)
{
    return (h && (h->base[ESP32C3_UART_LSR] & ESP32C3_UART_LSR_THRE)) ? 1 : 0;
}

int uart_hal_rx_pending(uart_hal_handle_t *h)
{
    return (h && (h->base[ESP32C3_UART_LSR] & ESP32C3_UART_LSR_DR)) ? 1 : 0;
}

int uart_hal_ore_pending(uart_hal_handle_t *h)
{
    /* 16550 LSR bit1 = OE (overrun error). Reading LSR clears it. */
    return (h && (h->base[ESP32C3_UART_LSR] & 0x02u)) ? 1 : 0;
}

void uart_hal_write_dr(uart_hal_handle_t *h, char c)
{
    if (!h) return;
    h->base[ESP32C3_UART_THR] = (uint8_t)c;
}

void uart_hal_enable_idle_irq(uart_hal_handle_t *h)  { (void)h; }
void uart_hal_disable_idle_irq(uart_hal_handle_t *h) { (void)h; }
int  uart_hal_idle_pending(uart_hal_handle_t *h)     { (void)h; return 0; }
void uart_hal_clear_idle(uart_hal_handle_t *h)       { (void)h; }

void uart_hal_clear_errors(uart_hal_handle_t *h)
{
    /* reading LSR clears the sticky OE/PE/FE/BI bits */
    if (h) (void)h->base[ESP32C3_UART_LSR];
}

uint32_t uart_hal_get_brr(uart_hal_handle_t *h)
{
    /* read back the current divisor (BRR analog) */
    return h ? uart_divisor(h->baud) : 0UL;
}

uint32_t uart_hal_get_cr1(uart_hal_handle_t *h)
{
    /* read back LCR (CR1 analog) */
    return h ? (uint32_t)h->base[ESP32C3_UART_LCR] : 0UL;
}

void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv)
{
    /* no hardware inversion on the 16550 (and Renode model ignores it) */
    (void)h; (void)rx_inv; (void)tx_inv;
}

void *uart_hal_get_dr_addr(uart_hal_handle_t *h)
{
    /* RBR/THR share offset 0 — the byte the DMA would push/pull */
    return h ? (void *)&h->base[ESP32C3_UART_RBR] : NULL;
}

void uart_hal_enable_tx_dma(uart_hal_handle_t *h)  { (void)h; }
void uart_hal_disable_tx_dma(uart_hal_handle_t *h) { (void)h; }
void uart_hal_enable_rx_dma(uart_hal_handle_t *h)  { (void)h; }
void uart_hal_disable_rx_dma(uart_hal_handle_t *h) { (void)h; }

uint32_t uart_hal_get_sr(uart_hal_handle_t *h)
{
    return h ? (uint32_t)h->base[ESP32C3_UART_LSR] : 0UL;
}

uint32_t uart_hal_get_cr3(uart_hal_handle_t *h)
{
    (void)h;
    return 0UL;     /* no CR3 equivalent on the 16550 */
}
