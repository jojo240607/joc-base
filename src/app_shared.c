#include "app_shared.h"
#include "console.h"

volatile uint32_t g_heartbeat = 0;
volatile int      g_rtos_demo_ready = 0;

/* 编译期把心跳/就绪标志地址钉进上下文：blink 在任何任务运行前解引用都不会是
 * NULL；设备句柄则在 app_main 任务里 open 后填进各字段。 */
app_ctx_t g_app_ctx = {
    .heartbeat  = &g_heartbeat,
    .demo_ready = &g_rtos_demo_ready,
};
