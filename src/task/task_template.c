#include "task_template.h"
#include "console.h"      /* app_ctx_t */
#include "app_shared.h"   /* g_app_ctx */
#include "rtos.h"

RTOS_TASK_STACK(g_mytask_stack, 1024);

/* 普通任务：周期/事件驱动逻辑都写在这里。示例为 1s 节拍。
 * 取设备句柄两种方式：走 app_ctx（c->led / c->adc / ...）或
 * device_manager_get("xxx")。 */
void mytask_task(void *arg)
{
    app_ctx_t *c = (app_ctx_t *)arg;
    (void)c;   /* 按需使用 c->led / c->adc / c->temp ... */

    for (;;) {
        /* ---- 你的任务逻辑 ---- */
        rtos_msleep(1000);
    }
}

/* 段注册：栈 + 入口同文件自管。
 * 末参 priv：1=特权（默认，可直接碰外设，适合绝大多数任务）；
 *            0=非特权用户态任务，内核自动经 SVC 门访问对象（见 RTOSUSR）。
 * 优先级 20 介于 MAIN(16) 与 BIST(24) 之间，数值越小越高，按需调整。 */
RTOS_TASK(mytask, "mytask", mytask_task, 20, g_mytask_stack, sizeof(g_mytask_stack), &g_app_ctx, 1);
