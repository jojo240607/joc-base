#ifndef SYSTEM_INIT_H
#define SYSTEM_INIT_H

/* 系统早期初始化：在 rtos_start() 之前、任何任务运行之前，把硬件与内核准备好。
 * 拆成独立文件，使 main() 只做“早期初始化 + 启动调度器”两件事（任务由
 * RTOS_TASK 段注册宏在 rtos_start() 内自动实例化，无需在此手动 rtos_task_create）。 */
void system_early_init(void);

#endif /* SYSTEM_INIT_H */
