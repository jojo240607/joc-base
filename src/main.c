/**
 * STM32F4 Discovery (STM32F407VGT6) minimal OOC example — running on jOS RTOS.
 *
 * 启动流程：
 *   Reset -> system_early_init()（建好所有 device + 起 1kHz systick + rtos_init）
 *        -> rtos_start() 内遍历 RTOS_TASK 段，自动实例化各任务
 *        -> 切到首个任务。
 *
 * 代码组织（每个关注点一个文件，main 只留入口）：
 *   - src/task/  : 各应用任务（blink/idle/bist/app_main/button + template），
 *                  每个文件自管栈 + 自带 RTOS_TASK 段注册；
 *   - app_shared.c: 跨任务共享状态（g_app_ctx 实例 + 心跳/就绪标志）；
 *   - console.c  : 命令解释器（命令表 + handler），由 app_main 调 console_run；
 *   - system_init.c: 早期硬件/内核初始化。
 * 加一个用户任务 = 在 src/task/ 写 task_xxx.c（函数 + 栈 + RTOS_TASK 一行），
 * 复制 task_template.c 改名最快；priv 传 0 即非特权用户态。
 */
#include "system_init.h"
#include "rtos.h"

int main(void)
{
    /* 仅做最小硬件初始化 + 启动 RTOS；任务由 RTOS_TASK 段宏在 rtos_start() 自动建。 */
    system_early_init();
    rtos_start();   /* 切换到首个任务；此线程上下文被丢弃，不再返回 */
    for (;;) { }    /* 保险：rtos_start 不会返回 */
}
