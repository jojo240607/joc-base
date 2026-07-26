#ifndef TASK_BUTTON_H
#define TASK_BUTTON_H

/* 按键中断示例：展示中断“上半部 / 下半部”如何在一个任务文件里接线。
 *   - 上半部 = exti 驱动的 ISR（Handler 模式，永远特权）：清挂起位后调用订阅回调
 *             button_isr_cb，它只做一件事——rtos_bh_trigger 唤醒下半部（ISR 安全）。
 *   - 下半部 = 专属高优先级 BH 任务 button_bh_fn（任务模式）：翻转 LED、打印，
 *             随时可被更高优先级 IRQ/任务抢占；单个按键中断绝不会卡死系统。
 * 详见 docs/rtos-design.md 第 4 章“中断上半部 / 下半部”。 */
void button_task(void *arg);

#endif /* TASK_BUTTON_H */
