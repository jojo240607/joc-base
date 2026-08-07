#ifndef TASK_DEMO_H
#define TASK_DEMO_H

#include "app_shared.h"        /* app_ctx_t */

/* 用户层演示 / 测试任务（干净默认配置下唯一的应用任务集合）。
 * 设计意图：展示“从用户层写几个测试 task demo”——覆盖
 *   - 特权周期任务（LED 翻转 + 心跳计数，证明抢占）
 *   - 特权周期任务（msleep 阻塞 + log 心跳）
 *   - 非特权用户态任务（priv=0，经 SVC 门做 IPC：sem 等待/释放）
 *   - 运行时由控制台 DEMO 命令动态创建的任务（rtos_task_create）
 * 这些任务在 rtos_start() 经 RTOS_TASK 段自动实例化；动态任务走 rtos_task_create。
 */

void demo_led_task(void *arg);     /* 周期翻转 LED + 心跳（取代原 blink） */
void demo_hello_task(void *arg);   /* 周期 log “hello”，演示 msleep 阻塞 */
void demo_user_task(void *arg);    /* 非特权任务：经 SVC 门做 IPC（sem wait） */
void demo_dynamic_task(void *arg); /* 由 DEMO 控制台命令动态创建的示例任务 */

/* DEMO 控制台命令：动态创建一个任务，证明用户层可在运行时建任务。 */
void cmd_demo(app_ctx_t *c, const char *line);

#endif /* TASK_DEMO_H */
