/**
 * 系统早期初始化（从 main() 抽出，使 main() 只剩“早期初始化 + 启动调度器”）。
 *
 * 这些硬件/内核准备工作必须在任何任务运行之前完成一次：板级设备建好、1 kHz
 * systick 起好、RTOS 内核初始化并在 systick 线注册节拍。任务本身由 RTOS_TASK
 * 段注册宏在 rtos_start() 内自动实例化，无需在此手动创建。
 */
#include "board.h"
#include "rtos.h"

void system_early_init(void)
{
    board_init();        /* 建好所有 device 并注册进 devmgr */
    board_tick_init();   /* 起 1 kHz systick */
    rtos_init();         /* 初始化内核并在 systick 线注册节拍 */
}
