#ifndef JOC_TASK_BUTTON_WQ_H
#define JOC_TASK_BUTTON_WQ_H

#include <stdint.h>

/* 按键中断示例（工作队列版）—— 演示中断"上半部 / 下半部"的另一种接法：
 * 下半部不新建专属任务，而是把工作挂入 RTOS 共享工作队列，由内核自带的
 * "wq" worker 任务（RTOS_PRIO_BH_MED）执行。省一个任务栈，适合"偶发、轻量、
 * 不要求最高优先级"的中断处理。
 *
 * 接线与 task_button.c（BH 任务版）完全对称，仅引脚/设备名不同：
 *   - 上半部：exti ISR -> button2_isr_cb（仅 rtos_work_submit，ISR 安全）
 *   - 下半部：button2_work_fn，运行在共享 wq 任务，而非本任务
 *   - 本任务 button_wq_task：建设备、订阅回调、武装 NVIC，之后阻塞让出 CPU
 *
 * 板子须注册名为 "btn2" 的 exti 设备（见 board.c，PA3 / EXTI line3）。
 * 控制台命令：BTN2（软件触发边沿）/ BTN2C（读中断计数）。 */
void button_wq_task(void *arg);

/* 读取按键中断累计次数（供 BTN2C 命令用）。 */
uint32_t button2_press_count(void);

#endif /* JOC_TASK_BUTTON_WQ_H */
