#ifndef TASK_BLINK_H
#define TASK_BLINK_H

/* 高优先级演示任务：g_rtos_demo_ready 置位前只在等标志（不碰 LED，避免干扰 LED
 * 自测），之后每 500ms 翻转 LED 并递增心跳计数，证明高优先级任务能抢占 main。 */
void blink_task(void *arg);

#endif /* TASK_BLINK_H */
