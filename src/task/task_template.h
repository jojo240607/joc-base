#ifndef TASK_TEMPLATE_H
#define TASK_TEMPLATE_H

/* 普通任务模板：把本文件复制一份改名（task_xxx.h / task_xxx.c）即可新建一个任务。
 * 约定（见 docs/rtos-design.md 第 5 章“应用任务组织”）：
 *   - 每个任务文件自管栈（RTOS_TASK_STACK）+ 自带 RTOS_TASK 段注册；
 *   - 跨任务共享状态放 app_shared.c（如 g_app_ctx 实例）；
 *   - 任务入口经 arg 拿到 app_ctx_t，不反向依赖 main.c 的全局变量。
 * 加任务后无需改 main.c；CMakeLists 已列出本目录文件，自动进构建。 */
void mytask_task(void *arg);

#endif /* TASK_TEMPLATE_H */
