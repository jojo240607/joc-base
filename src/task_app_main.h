#ifndef TASK_APP_MAIN_H
#define TASK_APP_MAIN_H

/* 原裸机 main() 的全部逻辑，现作为 "main" 任务运行（由 RTOS_TASK 段宏自动创建）。
 * 只负责 OPEN 设备 + 起 BIST（bist 任务）+ 填应用上下文 + 起控制台；
 * 设备拉起（board_init/rtos_init）已由 system_early_init() 在 rtos_start() 前完成。 */
void app_main_task(void *arg);

#endif /* TASK_APP_MAIN_H */
