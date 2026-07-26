#include "task_blink.h"
#include "console.h"
#include "app_shared.h"
#include "rtos.h"
#include "iface/device.h"
#include "drv/gpio_pin.h"

/* 栈（静态分配，2 的幂 + 对齐，启用 MPU 每任务栈 region(R4)：不可执行 XN）。
 * RTOS_TASK_STACK 同时保证对齐，否则自动退回软件哨兵（不误 fault）。 */
RTOS_TASK_STACK(g_blink_stack, 1024);

void blink_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    while (!*c->demo_ready) rtos_msleep(10);
    for (;;) {
        if (c->led) c->led->vtable->ioctl(c->led, GPIO_IOCTL_TOGGLE, (void *)0);
        (*c->heartbeat)++;
        rtos_msleep(500);
    }
}

/* 段收集模板注册（见 docs/rtos-design.md 第 5 章）：栈 + 任务函数同文件自管。
 * 末参 priv=1 特权（与历史一致）；非特权用户态任务传 0 即可自动走 SVC 门。 */
RTOS_TASK(blink, "blink", blink_task, RTOS_PRIO_BLINK, g_blink_stack, sizeof(g_blink_stack), &g_app_ctx, 1);
