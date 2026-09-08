/*
 * BOARD: ESP32-C3 DevKit (RISC-V RV32IMC @ 40 MHz, 192 KB IRAM, 192 KB DRAM)
 *
 * Phase-1 (Renode): minimal board — clock + UART0 only.
 *   - UART0 is an NS16550 model at 0x60000000 (no ESP32-C3 UART model in
 *     Renode; see hal/esp32c3/uart_hal.c), IRQ = PLIC source 13 -> id 29.
 *   - No pinmux / gpio / dma devices: the uart driver skips pin claims and
 *     DMA when the devices are absent (DMA_REQ_NONE + no "pinmux" node).
 *   - The 1 kHz RTOS tick comes from the CLINT machine timer (arch layer,
 *     rtos_init -> rtos_arch_tick_start); no board systick driver exists,
 *     so board_ticks() mirrors the kernel tick counter.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "device/esp32c3.h"     /* ESP32C3_UART0_BASE (board layer only) */
#include "irq.h"                /* platform-independent interrupt API */

#include "drv/clock.h"
#include "clock_hal.h"          /* clock_hal_configure() */

#include "drv/uart.h"
#include "rtos.h"               /* rtos_tick_count() */

/* ---- board-level tick service: the kernel tick is the board tick ---- */
void board_tick_init(void)
{
    /* Bring the (fixed 40 MHz) core clock up — idempotent NOP under Renode.
     * The RTOS tick itself is armed by rtos_init() via the arch layer
     * (CLINT mtimecmp), so nothing else is needed here. */
    clock_hal_configure();
}

uint32_t board_ticks(void)
{
    return rtos_tick_count();
}

/* ---- board devices as DATA ---- */
static const uart_config_t g_uart0 = {
    .name       = "uart0",
    .periph     = (void *)ESP32C3_UART0_BASE,   /* 0x60000000 (NS16550 model) */
    .baud       = 115200,
    .is_console = 1,
    .tx_signal  = "UART0_TX",       /* GPIO20 — unused Phase-1 (no pinmux) */
    .rx_signal  = "UART0_RX",       /* GPIO21 — unused Phase-1 (no pinmux) */
    .dma_tx_req = DMA_REQ_NONE,     /* IRQ TX (no peripheral DMA under Renode) */
    .dma_rx_req = DMA_REQ_NONE,
    .engine     = STREAM_MODE_IRQ,
    .framing    = UART_FRAME_NONE,
};

static const clock_config_t g_clk  = { "clk" };

static const board_node_t g_nodes[] = {
    { clock_create, &g_clk },
    { uart_create,  &g_uart0 },
};

static device *board_build(const board_node_t *n)
{
    return n->create(n->config);
}

void board_init(void)
{
    for (uint32_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        device *dev = board_build(&g_nodes[i]);
        if (dev) device_manager_register(device_get_name(dev), dev);
    }
}
