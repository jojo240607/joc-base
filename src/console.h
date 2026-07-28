#ifndef CONSOLE_H
#define CONSOLE_H

#include "iface/device.h"
#include "rtos_config.h"          /* RTOS_SELFTEST 开关：决定 st 字段 / selftest.h 是否参与 */

#if RTOS_SELFTEST
#include "selftest.h"
#endif

/* 命令处理器共享的“应用上下文”：把 main 里散落的全局句柄/状态打包成一个结构体，
 * 通过参数传给每个命令 handler，避免 console.c 反向依赖 main.c 的全局变量。
 * 设备句柄在 app_main_task 里取好后填进本结构；状态指针指向 main.c 的静态量。 */
typedef struct app_ctx {
    device *uart;        /* 调试 UART（默认控制台来源） */
    device *usb;         /* USB CDC-ACM 控制台（可选） */
    device *led;         /* LED 设备（blink 任务用） */
    device *adc;         /* ADC 设备 */
    device *temp;        /* 温度传感器设备 */
    device *clk;         /* 时钟设备 */
    device *console;     /* 当前响应端口：读循环随输入来源在 UART/USB 间切换 */
#if RTOS_SELFTEST
    selftest *st;        /* BIST 句柄（由 bist 任务填充，BIST 命令读取重跑） */
#endif
    volatile uint32_t *heartbeat;  /* 指向 app_shared.c 的 g_heartbeat（RTOS 命令读取） */
    volatile int      *demo_ready; /* 指向 app_shared.c 的 g_rtos_demo_ready（blink 等待） */
} app_ctx_t;

/* 命令 handler：c = 应用上下文，line = 整条命令（含参数，handler 自行解析）。 */
typedef void (*cmd_fn)(app_ctx_t *c, const char *line);

/* 命令表条目：prefix=1 表示“命令词 + 空格 + 参数”的前缀匹配（ECHO/ADC/USBDBG），
 * prefix=0 表示整行精确匹配（PING/RTOS/TICKS ...）。 */
typedef struct {
    const char *name;
    cmd_fn      fn;
    int         prefix;
} cmd_entry_t;

/* 命令解释器主循环：从 uart/usb 读行 -> 查表派发 -> 调对应 handler。
 * 永不返回（for(;;) 阻塞读，空闲时让出 1ms）。 */
void console_run(app_ctx_t *c);

#endif /* CONSOLE_H */
