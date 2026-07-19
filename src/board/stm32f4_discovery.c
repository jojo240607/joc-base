/*
 * BOARD: STM32F4 Discovery (STM32F407VGT6)
 *
 * This is the ONLY file (besides the HAL) that knows the real silicon:
 * ADC1, USART1, GPIOD and the factory temperature calibration words. The
 * hardware assignment is expressed as DATA: each device is an instance of its
 * driver's OWN config struct, filled here. board_init() walks the node array,
 * calls each node's create fn (which takes ONLY the config pointer) and
 * registers the device by the name the driver set. There is NO switch and NO
 * per-driver probe / build wrapper in this file — the config + its create fn
 * IS the dispatch.
 *
 * To port to another board you write a new src/board/<board>.c with a different
 * config array + the matching HAL; main.c and the drv/ sources are untouched.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "stm32f4xx.h"          /* real peripherals — board layer only */
#include "irq.h"                /* platform-independent interrupt API */

#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/pinmux.h"

#include "temp_hal.h"

/* ---- board-level SysTick tick service, built as an EVENT device ------------
 * SysTick is modeled as an `event_device` (see drv/systick.c): the driver
 * configures the core timer and registers its ISR through the GENERIC irq
 * framework; the board subscribes a tick callback. This proves the four-class
 * model + the irq framework work together. main.c still calls board_tick_init()
 * / board_ticks(), so its TICKS command is unchanged. */
#include "drv/systick.h"

static volatile uint32_t g_board_ticks;

static void board_tick_cb(void *ctx, device_event_type_t ev, void *data)
{
    (void)ctx; (void)ev; (void)data;
    g_board_ticks++;
}

void board_tick_init(void)
{
    systick_config_t c;
    c.name    = "systick";
    c.cpu_hz  = SystemCoreClock;   /* board knows the real core clock */
    c.tick_hz = 1000;              /* 1 ms tick */
    device *d = systick_create(&c);
    if (d)
        device_manager_register("systick", d);
    event_device *e = device_as_event(d);
    if (e)
        e->vtable->set_event_callback(e, DEVICE_EVENT_TICK, board_tick_cb, NULL);
}

uint32_t board_ticks(void) { return g_board_ticks; }

/* ---- board devices as DATA (each driver's own config, filled by the board) */
static const pinmux_config_t g_pinmux = { "pinmux" };
static const adc_config_t  g_adc0  = { "adc0",  (void *)ADC1, 0, 3300,
                                       "ADC1_IN0" };          /* PA0, af=0 */
static const uart_config_t g_uart0 = { "uart0", (void *)USART1, 115200, 1,
                                       "USART1_TX_PA9",        /* TX = PA9, AF7 */
                                       "USART1_RX_PA10" };     /* RX = PA10, AF7 */
static const gpio_config_t g_led   = { "led",   "GPIOD_12", 1 }; /* D12, output */
static const clock_config_t g_clk  = { "clk" };
static const temp_config_t g_temp0 = { "temp0", "adc0", 3300 };   /* adc0 must precede temp0 */

/* the board is just a list of (create-fn, config) pairs — no type switch.
 * pinmux is listed FIRST so it is registered before any driver claims pins.
 * Each driver claims and configures its own pins through the pinmux at open()
 * time, using the SIGNAL NAMES supplied above. The pinmux resolves each name
 * to its exact (port, pin, af) — so a pin conflict is rejected before any GPIO
 * register is touched, instead of being logged here. The board never lists a
 * raw (port, pin, af) triple: the AF database is the single source of truth. */
static const board_node_t g_nodes[] = {
    { pinmux_create,      &g_pinmux },
    { clock_create,       &g_clk },
    { uart_create,        &g_uart0 },
    { gpio_pin_create,    &g_led },
    { adc_create,         &g_adc0 },
    { temp_sensor_create, &g_temp0 },
};

/* generic dispatcher — forwards ONLY the config pointer, no switch */
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
