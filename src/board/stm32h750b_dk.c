/*
 * BOARD: STM32H750B-DK (STM32H750VBT6, Cortex-M7 @ 480 MHz, 128 KB Flash,
 *        128 KB DTCM, 512 KB AXI SRAM)
 *
 * Milestone 2: DMA1/DMA2 + UART0 DMA TX/RX (silicon DMAMUX).
 * Renode: no peripheral DMA (USART1 IRQ), dma2 M2M selftest via RTOSDMA cmd.
 * D-Cache enabled with MPU SRAM1 non-cacheable heap for DMA coherence.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "stm32h7xx.h"          /* real peripherals — board layer only */
#include "irq.h"                /* platform-independent interrupt API */

#include "drv/clock.h"
#include "clock_hal.h"          /* clock_hal_configure() */

#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/pinmux.h"

#include "drv/dma.h"
#include "drv/qspi_flash.h"
#include "drv/sdio.h"
#include "drv/sd_card.h"

#include "drv/adc.h"
#include "drv/temp_sensor.h"

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
    /* Bring core clock to PLL1@480MHz first (idempotent; also rewrites
     * SystemCoreClock so SysTick reload uses the real CPU frequency). */
    clock_hal_configure();

    systick_config_t c;
    c.name    = "systick";
    c.cpu_hz  = clock_hal_sysclk_hz();   /* 480 MHz */
    c.tick_hz = 1000;                    /* 1 ms tick */
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

/* DMA controllers — dma1 (M2P/P2M only) and dma2 (M2M-capable) */
#if defined(JOC_RENODE)
/* Renode: USART1 stays IRQ (F4-style DMA model lacks uart-periph routing).
 * DMA controllers registered only for RTOSDMA M2M selftest (dma2). */
static const uart_config_t g_uart0 = {
    .name       = "uart0",
    .periph     = (void *)USART1,
    .baud       = 115200,
    .is_console = 1,
    .tx_signal  = "USART1_TX",         /* TX = PA9, AF7 (ST-LINK VCOM) */
    .rx_signal  = "USART1_RX",         /* RX = PA10, AF7 */
    .dma_tx_req = DMA_REQ_NONE,        /* IRQ TX (no peripheral DMA on Renode) */
    .dma_rx_req = DMA_REQ_NONE,
    .engine     = STREAM_MODE_IRQ,
    .framing    = UART_FRAME_NONE,
};
#else
/* Silicon: USART1 TX/RX via DMAMUX (dma1 streams 0/1, DMAREQ_ID 37/38). */
static const uart_config_t g_uart0 = {
    .name       = "uart0",
    .periph     = (void *)USART1,
    .baud       = 115200,
    .is_console = 1,
    .tx_signal  = "USART1_TX",         /* TX = PA9, AF7 (ST-LINK VCOM) */
    .rx_signal  = "USART1_RX",         /* RX = PA10, AF7 */
    .dma_tx_req = DMA_REQ_USART1_TX,
    .dma_rx_req = DMA_REQ_USART1_RX,
    .engine     = STREAM_MODE_DMA,
    .framing    = UART_FRAME_NONE,
};
#endif

/* DMA1 — 8 streams, no M2M */
static const dma_config_t g_dma1 = { "dma1", (void *)DMA1 };

/* DMA2 — 8 streams, M2M-capable (used by RTOSDMA M2M selftest on Renode) */
static const dma_config_t g_dma2 = { "dma2", (void *)DMA2 };

/* External QUADSPI flash (W25Q128, APP_FLASH partition, 0x90000000 XIP).
 * Real-silicon programming (indirect mode); Renode has no QUADSPI model so
 * all operations fail there (qspi_hal.c returns -1 under JOC_RENODE). */
static const qspi_flash_config_t g_qspi = { "qspi0" };

/* PB0 = LD1 (green) on the STM32H750B-DK board, active high */
static const gpio_config_t g_led   = { "led", "GPIOB_0", 1 };

static const clock_config_t g_clk  = { "clk" };

/* ADC1_IN0 = PA0 (same as F4 Discovery), 3300 mV, DMA via dma2 stream4 DMAMUX ID1 */
static const adc_config_t  g_adc0  = { "adc0",  (void *)ADC1, 0, 3300,
                                       "ADC1_IN0", DMA_REQ_ADC1 };

/* Temperature sensor on ADC1 internal channel 16, 3300 mV */
static const temp_config_t g_temp0 = { "temp0", "adc0", 3300 };

/* SDMMC1 (SDIO): PC8-12 + PD2, AF12, same pins as F4 SDIO */
static const sdio_config_t g_sdio0 = {
    "sdio0", (void *)SDMMC1,
    "SDIO_CK", "SDIO_CMD",
    "SDIO_D0", "SDIO_D1", "SDIO_D2", "SDIO_D3",
    DMA_REQ_SDIO
};

/* SD Card on sdio0 bus, slot 0 */
static const sd_card_config_t g_sd_card0 = { "sd_card0", "sdio0", 0 };

static const board_node_t g_nodes[] = {
    { pinmux_create,      &g_pinmux },
    { clock_create,       &g_clk },
    { dma_create,         &g_dma1 },
    { dma_create,         &g_dma2 },
    { qspi_flash_create,  &g_qspi },
    { uart_create,        &g_uart0 },
    { gpio_pin_create,    &g_led },
    { adc_create,         &g_adc0 },
    { temp_sensor_create, &g_temp0 },
    { sdio_create,        &g_sdio0 },
    { sd_card_create,     &g_sd_card0 },
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
