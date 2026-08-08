#include "app_shared.h"
#include "console.h"

volatile uint32_t g_heartbeat = 0;

/* 编译期把心跳标志地址钉进上下文；设备句柄在 app_main 任务里 open 后填进各字段。 */
app_ctx_t g_app_ctx = {
    .heartbeat  = &g_heartbeat,
};
