/*
 * BOARD: STM32F4 Discovery (STM32F407VGT6)
 *
 * This is the ONLY file (besides the HAL) that knows the real silicon:
 * ADC1, USART1, GPIOD and the factory temperature calibration words. The
 * hardware assignment is expressed as const DATA (the "device tree" blob) and
 * board_init() walks it, constructs the right HAL handle + driver for each
 * node, and registers the result into the device manager under its name.
 *
 * To port to another board you write a new src/board/<board>.c with a
 * different descriptor + the matching HAL; main.c and the drv/ sources are
 * untouched.
 */
#include "board.h"
#include "devmgr/device_manager.h"

#include "stm32f4xx.h"          /* real peripherals — board layer only */

#include "drv/clock.h"
#include "drv/uart.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"

#include "adc_hal.h"
#include "gpio_hal.h"
#include "uart_hal.h"
#include "temp_hal.h"

/* ---- board resources as DATA (device-tree equivalent) ----------------- */
static const adc_board_res_t  g_adc0  = { "adc0",  (void *)ADC1, 0, 3300 };
static const uart_board_res_t g_uart0 = { "uart0", (void *)USART1, 115200, 1 };
static const gpio_board_res_t g_led   = { "led",   (void *)GPIOD, 12, 1 };
static const clock_board_res_t g_clk  = { "clk" };
static const temp_sensor_board_res_t g_temp0 = { "temp0", "adc0", 3300 };

static const board_node_t g_nodes[] = {
    { BOARD_DEV_CLOCK,       &g_clk  },
    { BOARD_DEV_UART,        &g_uart0 },
    { BOARD_DEV_GPIO,        &g_led  },
    { BOARD_DEV_ADC,         &g_adc0 },
    { BOARD_DEV_TEMP_SENSOR, &g_temp0 },   /* adc0 must precede temp0 */
};

static const board_desc_t g_board_desc = {
    g_nodes, sizeof(g_nodes) / sizeof(g_nodes[0])
};

void board_init(void)
{
    for (uint32_t i = 0; i < g_board_desc.node_count; i++) {
        const board_node_t *n = &g_board_desc.nodes[i];
        switch (n->type) {
        case BOARD_DEV_CLOCK:
            device_manager_register("clk", (device *)clock_create());
            break;

        case BOARD_DEV_UART: {
            const uart_board_res_t *r = (const uart_board_res_t *)n->res;
            uart_hal_handle_t *h = uart_hal_create(r->peripheral, r->baud);
            uart *u = uart_create(h);
            if (r->is_console) uart_set_console(u);
            device_manager_register(r->name, (device *)u);
            break;
        }

        case BOARD_DEV_GPIO: {
            const gpio_board_res_t *r = (const gpio_board_res_t *)n->res;
            gpio_hal_handle_t *h = gpio_hal_create(r->peripheral, r->pin, r->mode);
            device_manager_register(r->name, (device *)gpio_pin_create(h));
            break;
        }

        case BOARD_DEV_ADC: {
            const adc_board_res_t *r = (const adc_board_res_t *)n->res;
            adc_hal_handle_t *h = adc_hal_create(r->peripheral, r->channel);
            device_manager_register(r->name, (device *)adc_create(h, r->channel));
            break;
        }

        case BOARD_DEV_TEMP_SENSOR: {
            const temp_sensor_board_res_t *r =
                (const temp_sensor_board_res_t *)n->res;
            device *adc = device_manager_get(r->adc_name);
            temp_sensor *t = temp_sensor_create(adc, r->vdda_mv,
                                                 temp_hal_ts_cal1(),
                                                 temp_hal_ts_cal2());
            device_manager_register(r->name, (device *)t);
            break;
        }

        default:
            break;
        }
    }
}
