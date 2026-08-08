#ifndef APP_SHARED_H
#define APP_SHARED_H

#include "console.h"   /* app_ctx_t */

/* 应用级共享状态：原本散落在 main.c 的全局，现集中于此，供各任务文件经
 * RTOS_TASK 的 arg（&g_app_ctx）共享，避免任务文件反向依赖 main.c。
 * 加新任务时，若需要跨任务共享的全局状态，也放这里（并 extern 声明）。 */
extern volatile uint32_t g_heartbeat;       /* 1Hz 心跳计数（RTOS 命令读取） */
extern app_ctx_t         g_app_ctx;          /* 应用上下文实例（由 app_main 任务填充） */

#endif /* APP_SHARED_H */
