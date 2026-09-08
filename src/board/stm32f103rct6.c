/*
 * BOARD: STM32F103RCT6 (Cortex-M3, 72 MHz, 256 KB Flash, 48 KB SRAM)
 *
 * Minimal port: only clock, UART (USART1 console), GPIO (PC13 LED), pinmux.
 * No DMA, no USB, no ADC, no CAN, no SDIO, no timer peripherals.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "stm32f1xx.h"          /* real peripherals — board layer only */
#include "irq.h"                /* platform-independent interrupt API */

#include "drv/clock.h"
#include "clock_hal.h"          /* clock_hal_configure() */

#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/pinmux.h"

/* ---- board-level SysTick tick service ---- */
#include "drv/systick.h"

static volatile uint32_t g_board_ticks;

static void board_tick_cb(void *ctx, device_event_type_t ev, void *data)
{
    (void)ctx; (void)ev; (void)data;
    g_board_ticks++;
}

void board_tick_init(void)
{
    /* Bring core clock to PLL@72MHz first */
    clock_hal_configure();

    systick_config_t c;
    c.name    = "systick";
    c.cpu_hz  = clock_hal_sysclk_hz();
    c.tick_hz = 1000;              /* 1 ms tick */
    device *d = systick_create(&c);
    if (d)
        device_manager_register("systick", d);
    event_device *e = device_as_event(d);
    if (e)
        e->vtable->set_event_callback(e, DEVICE_EVENT_TICK, board_tick_cb, NULL);
}

uint32_t board_ticks(void) { return g_board_ticks; }

/* ---- board devices as DATA ---- */
static const pinmux_config_t g_pinmux = { "pinmux" };

static const uart_config_t g_uart0 = {
    .name       = "uart0",
    .periph     = (void *)USART1,
    .baud       = 115200,
    .is_console = 1,
    .tx_signal  = "USART1_TX",         /* TX = PA9 */
    .rx_signal  = "USART1_RX",         /* RX = PA10 */
    .dma_tx_req = DMA_REQ_NONE,        /* IRQ TX (no DMA on minimal port) */
    .dma_rx_req = DMA_REQ_NONE,
    .engine     = STREAM_MODE_IRQ,     /* Interrupt-driven TX/RX */
    .framing    = UART_FRAME_NONE,
};

/* PC13 = on-board LED (many F103 boards) */
static const gpio_config_t g_led   = { "led", "GPIOC_13", 1 };

static const clock_config_t g_clk  = { "clk" };

static const board_node_t g_nodes[] = {
    { pinmux_create,      &g_pinmux },
    { clock_create,       &g_clk },
    { uart_create,        &g_uart0 },
    { gpio_pin_create,    &g_led },
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