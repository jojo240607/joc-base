#include "task_bist.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "iface/device.h"
#include "devmgr/device_manager.h"
#include "log/log.h"
#include "log/app_log.h"
#include "selftest.h"
#include "board.h"
#include "drv/clock.h"
#include "drv/gpio_pin.h"
#include "drv/adc.h"
#include "drv/temp_sensor.h"
#include "drv/pinmux.h"

RTOS_TASK_STACK(g_bist_stack, 3072);  /* BIST 后台任务栈 */

void bist_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    device *d_clk  = device_manager_get("clk");
    device *d_uart = device_manager_get("uart0");
    device *d_led  = device_manager_get("led");
    device *d_adc  = device_manager_get("adc0");
    device *d_temp = device_manager_get("temp0");

    c->st = selftest_create(d_clk, d_uart, d_led, d_adc, d_temp);
    selftest_run(c->st);

    /* pinmux conflict-detection self-test (exercises the new driver) */
    device *d_pinmux = device_manager_get("pinmux");
    int pmok = pinmux_run_selftest((pinmux *)d_pinmux);
    log_printf(app_log(), LOG_INFO, "main", "[BIST] pinmux: %s\n", pmok ? "PASS" : "FAIL");

    for (;;) rtos_yield();   /* BIST 完成(或卡死在上面)；在此安静让出 */
}

RTOS_TASK(bist, "bist", bist_task, RTOS_PRIO_BIST, g_bist_stack, sizeof(g_bist_stack), &g_app_ctx, 1);
