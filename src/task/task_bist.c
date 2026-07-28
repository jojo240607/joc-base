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

    /* BIST 完成后本任务无事可做：必须【阻塞】而非忙等 rtos_yield()。
     * rtos_yield() 只让给“同级或更高优先级”任务，若此处忙等，则任何优先级
     * 低于 bist(24) 的任务(如 prio=28/31)将永得不到 CPU，造成人为饿死。
     * 改用 msleep 阻塞：任务进入阻塞态，调度器才会选更低优先级就绪任务运行，
     * 既空出 CPU 又保持“BIST 后安静”。1s 周期唤醒仅用于佐证调度器存活。 */
    for (;;) rtos_msleep(1000);
}

RTOS_TASK(bist, "bist", bist_task, RTOS_PRIO_BIST, g_bist_stack, sizeof(g_bist_stack), &g_app_ctx, 1);
