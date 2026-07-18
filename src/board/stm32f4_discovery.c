/*
 * BOARD: STM32F4 Discovery (STM32F407VGT6)
 *
 * This is the ONLY file (besides the HAL) that knows the real silicon:
 * ADC1, USART1, GPIOD and the factory temperature calibration words. The
 * hardware assignment is expressed as const DATA (the "device tree" blob) and
 * board_init() walks it. Construction is dispatched through a PER-CLASS PROBE
 * TABLE (g_probes[]) indexed by driver_type_t — so board_init() contains NO
 * switch on the device type; adding a new driver class means adding one entry
 * to the table, not touching the init loop.
 *
 * To port to another board you write a new src/board/<board>.c with a different
 * descriptor + the matching HAL; main.c and the drv/ sources are untouched.
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
static const adc_board_res_t  g_adc0  = { (void *)ADC1, 0, 3300 };
static const uart_board_res_t g_uart0 = { (void *)USART1, 115200, 1 };
static const gpio_board_res_t g_led   = { (void *)GPIOD, 12, 1 };
static const clock_board_res_t g_clk  = { 0 };
static const temp_sensor_board_res_t g_temp0 = { "adc0", 3300 };

static const board_node_t g_nodes[] = {
    { DEVICE_TYPE_CLOCK,       "clk",   &g_clk  },
    { DEVICE_TYPE_UART,        "uart0", &g_uart0 },
    { DEVICE_TYPE_GPIO,        "led",   &g_led  },
    { DEVICE_TYPE_ADC,         "adc0",  &g_adc0 },
    { DEVICE_TYPE_TEMP_SENSOR, "temp0", &g_temp0 },   /* adc0 must precede temp0 */
};

static const board_desc_t g_board_desc = {
    g_nodes, sizeof(g_nodes) / sizeof(g_nodes[0])
};

/* ---- per-class probe table (uniform signature: name + resource) -------
 * Each probe builds the right HAL handle + driver and returns a device *.
 * The driver itself fills device.type / device.name at init, so the manager
 * can later classify/lookup without knowing the concrete type. */
typedef device *(*driver_probe_t)(const char *name, const void *res);

static device *probe_clock(const char *name, const void *res)
{
    (void)res;
    return (device *)clock_create(name);
}

static device *probe_adc(const char *name, const void *res)
{
    const adc_board_res_t *r = (const adc_board_res_t *)res;
    adc_hal_handle_t *h = adc_hal_create(r->peripheral, r->channel);
    return (device *)adc_create(h, r->channel, name);
}

static device *probe_uart(const char *name, const void *res)
{
    const uart_board_res_t *r = (const uart_board_res_t *)res;
    uart *u = uart_create(uart_hal_create(r->peripheral, r->baud), name);
    if (r->is_console) uart_set_console(u);
    return (device *)u;
}

static device *probe_gpio(const char *name, const void *res)
{
    const gpio_board_res_t *r = (const gpio_board_res_t *)res;
    gpio_hal_handle_t *h = gpio_hal_create(r->peripheral, r->pin, r->mode);
    return (device *)gpio_pin_create(h, name);
}

static device *probe_temp(const char *name, const void *res)
{
    const temp_sensor_board_res_t *r = (const temp_sensor_board_res_t *)res;
    device *adc = device_manager_get(r->adc_name);   /* already probed above */
    return (device *)temp_sensor_create(adc, r->vdda_mv,
                                        temp_hal_ts_cal1(),
                                        temp_hal_ts_cal2(), name);
}

static const driver_probe_t g_probes[DEVICE_TYPE_COUNT] = {
    [DEVICE_TYPE_CLOCK]       = probe_clock,
    [DEVICE_TYPE_ADC]         = probe_adc,
    [DEVICE_TYPE_UART]        = probe_uart,
    [DEVICE_TYPE_GPIO]        = probe_gpio,
    [DEVICE_TYPE_TEMP_SENSOR] = probe_temp,
};

void board_init(void)
{
    for (uint32_t i = 0; i < g_board_desc.node_count; i++) {
        const board_node_t *n = &g_board_desc.nodes[i];
        driver_probe_t probe = (n->type < DEVICE_TYPE_COUNT) ? g_probes[n->type] : NULL;
        if (!probe) continue;                       /* unknown class -> skip */
        device *dev = probe(n->name, n->res);
        device_manager_register(n->name, dev);
    }
}
