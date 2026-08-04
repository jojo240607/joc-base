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

/* 调试触发标志：置 1 后 bist 任务下一拍自动跑 RTOSALL，结果写入 g_rtosall_result。
 * 用于无串口环境（gdb/OpenOCD）下判定自测结果；正常构建永不被置位。 */
volatile int g_dbg_auto_rtosall = 0;
extern volatile int g_rtosall_result;

/* BIST 栈统一 3K：selftest_run 深层调用需要。覆盖率构建曾砍到 1K 导致栈溢出；gcov 段已
 * 搬回主 SRAM，CCM 有余量，恢复正常 3K。 */
RTOS_TASK_STACK(g_bist_stack, 3072);


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
    for (;;) {
        /* 调试触发：OpenOCD/gdb 置位 g_dbg_auto_rtosall 后，下一拍自动跑一遍
         * RTOSALL 并把结果镜像到 g_rtosall_result（无串口时也可判定 PASS/FAIL）。 */
        if (g_dbg_auto_rtosall) {
            g_dbg_auto_rtosall = 0;
            /* 各子测会创建优先级高于 bist(24) 的任务(basic T03=18 / T04=6 /
             * T05=5、irq_a=3 等)。若直接以 bist 原优先级运行 rtos_selftest_run_all，
             * 这些更高优先级子任务会永久抢占 bist，使其被 msleep 唤醒后无法设置
             * stop 标志，造成饿死/死锁(实测卡在 basic/T03)。故运行前把 bist 的
             * 有效优先级临时顶到 RTOS_PRIO_RTOSALL_RUNNER(高于所有子测任务最高
             * 优先级 3)，跑完再恢复——RTOSALL 一次性运行期间短暂提升，不影响常态。 */
            uint8_t save_prio = rtos_task_raise_prio((uint8_t)RTOS_PRIO_RTOSALL_RUNNER);
            g_rtosall_result = rtos_selftest_run_all();
            rtos_task_restore_prio(save_prio);
        }
        rtos_msleep(1000);
    }
}

RTOS_TASK(bist, "bist", bist_task, RTOS_PRIO_BIST, g_bist_stack, sizeof(g_bist_stack), &g_app_ctx, 1);
