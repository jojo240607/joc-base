#ifndef UART_HAL_H
#define UART_HAL_H

#include <stdint.h>
#include "irq.h"              /* irq_id_t — the HAL returns the platform irq id */

/* Opaque handle type — the concrete struct is private to uart_hal.c */
typedef struct uart_hal_handle uart_hal_handle_t;

/*
 * Hardware Abstraction Layer — UART (ESP32-C3 implementation).
 *
 * The driver layer only handles the OPAQUE `uart_hal_handle_t *`; it never sees
 * any register. The concrete struct is private to uart_hal.c.
 * Porting to another chip = rewrite this HAL only.
 *
 * Phase-1 note (Renode): there is no ESP32-C3 UART model in Renode, so the
 * platform uses a UART.NS16550 model at the ESP32-C3 UART0 base (0x60000000).
 * This HAL therefore programs the 16550 register layout (byte-wide registers,
 * DLAB-gated divisor), see device/esp32c3.h. On real silicon this file is
 * replaced by the ESP32-C3 UART register implementation — same API.
 */

/* 16550 clock used for the baud divisor: 1.8432 MHz (classic value; Renode
 * model does not validate the divider, only 115200 is used in Phase-1). */
#define UART_HAL_BAUD_CLK_HZ 1843200UL

uart_hal_handle_t *uart_hal_create(void *peripheral, uint32_t baud);
void uart_hal_destroy(uart_hal_handle_t *h);

void uart_hal_init(uart_hal_handle_t *h);
void uart_hal_deinit(uart_hal_handle_t *h);
void uart_hal_set_baudrate(uart_hal_handle_t *h, uint32_t baud);
void uart_hal_set_parity(uart_hal_handle_t *h, int parity);   /* 0=none,1=odd,2=even */
void uart_hal_set_stopbits(uart_hal_handle_t *h, int stop);   /* 1 or 2 stop bits */
void uart_hal_putc(uart_hal_handle_t *h, char c);
char uart_hal_getc(uart_hal_handle_t *h);
uint32_t uart_hal_get_baudrate(uart_hal_handle_t *h); /* read back logical baud */

/* --- interrupt support (used by the driver via the platform-independent
 *     irq framework: it registers uart_hal_irq_id() with irq_register) --- */
char uart_hal_read_dr(uart_hal_handle_t *h);     /* read RBR (clears DR) */
void uart_hal_enable_rx_irq(uart_hal_handle_t *h);  /* set IER.ERBFI */
void uart_hal_disable_rx_irq(uart_hal_handle_t *h); /* clear IER.ERBFI */
irq_id_t uart_hal_irq_id(uart_hal_handle_t *h);    /* PLIC source 13 -> id 29 */
/* --- TX interrupt support (drive blocking writes / async submit from the TXE ISR) --- */
void uart_hal_enable_tx_irq(uart_hal_handle_t *h);  /* set IER.ETBEI */
void uart_hal_disable_tx_irq(uart_hal_handle_t *h); /* clear IER.ETBEI */
int  uart_hal_tx_ready(uart_hal_handle_t *h);       /* (LSR & THRE) != 0 */
int  uart_hal_rx_pending(uart_hal_handle_t *h);     /* (LSR & DR) != 0 */
int  uart_hal_ore_pending(uart_hal_handle_t *h);    /* (LSR & OE) != 0 — overrun freezes RX */
void uart_hal_write_dr(uart_hal_handle_t *h, char c);/* write THR (triggers TX) */

/* --- IDLE-line interrupt (variable-length DMA reception) ---
 * The 16550 has no IDLE detection (that is a USART feature); these are
 * no-ops returning "not pending" so the driver's IDLE framing path stays
 * linkable and behaves as "no frame end detected". */
void uart_hal_enable_idle_irq(uart_hal_handle_t *h);   /* no-op */
void uart_hal_disable_idle_irq(uart_hal_handle_t *h);  /* no-op */
int  uart_hal_idle_pending(uart_hal_handle_t *h);      /* 0 */
void uart_hal_clear_idle(uart_hal_handle_t *h);        /* no-op */
void uart_hal_clear_errors(uart_hal_handle_t *h);      /* read LSR (clears OE/PE/FE/BI) */
uint32_t uart_hal_get_brr(uart_hal_handle_t *h);   /* read back baud divisor for self-test */
uint32_t uart_hal_get_cr1(uart_hal_handle_t *h);   /* read back LCR for self-test */

/* --- logic-level inversion (no-op on 16550, no RXINV/TXINV bits) --- */
void uart_hal_set_inverted(uart_hal_handle_t *h, int rx_inv, int tx_inv);

/* --- DMA support (stub on ESP32-C3 Phase-1, no peripheral DMA on Renode) --- */
void    *uart_hal_get_dr_addr(uart_hal_handle_t *h);   /* &RBR/THR (for DMA PAR) */
void uart_hal_enable_tx_dma(uart_hal_handle_t *h);     /* no-op */
void uart_hal_disable_tx_dma(uart_hal_handle_t *h);    /* no-op */
void uart_hal_enable_rx_dma(uart_hal_handle_t *h);     /* no-op */
void uart_hal_disable_rx_dma(uart_hal_handle_t *h);    /* no-op */

/* readback getters for DMA debugging */
uint32_t uart_hal_get_sr(uart_hal_handle_t *h);        /* LSR */
uint32_t uart_hal_get_cr3(uart_hal_handle_t *h);       /* 0 (no CR3 equivalent) */

#endif /* UART_HAL_H */
