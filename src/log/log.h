#ifndef JOC_BASE_DS_LOG_H
#define JOC_BASE_DS_LOG_H

#include <stddef.h>
#include <stdarg.h>

#include "../bus/bus.h"            /* 内部事件总线：多 sink 经其订阅同一行日志 */

/* ---------------------------------------------------------------------------
 * 日志级别（数值越大越详细；LOG_NONE 表示全部关闭）
 * 与 ESP-IDF 的 esp_log_level_t 取值风格一致，便于后续对接。
 * ------------------------------------------------------------------------- */
typedef enum {
    LOG_NONE = 0,       /* 全部关闭 */
    LOG_EMERGENCY,      /* 系统不可用（最高优先级） */
    LOG_ALERT,          /* 必须立即处理 */
    LOG_CRITICAL,       /* 临界（对应 MAV_SEVERITY_CRITICAL） */
    LOG_ERROR,          /* 错误 */
    LOG_WARN,           /* 警告 */
    LOG_INFO,           /* 信息（默认阈值） */
    LOG_DEBUG,          /* 调试 */
    LOG_VERBOSE         /* 最详尽 */
} log_level_t;

/* 不透明日志对象（注意：类型名用 `logger` 而非 `log`，避免与 C 标准库 double log(double) 冲突） */
typedef struct log logger;

/* 输出回调：把最终字符串交给介质（UART / RTT / stdout …）。
 * level 为本条日志的级别（与 MAV_SEVERITY 等映射时透传，便于按级别路由）。 */
typedef void (*log_sink_fn)(void *ctx, log_level_t level, const char *data, size_t len);

#ifndef LOG_BUF_SIZE
#define LOG_BUF_SIZE 128   /* 单条日志格式化缓冲上限，可按需覆盖 */
#endif

#ifndef LOG_MAX_SINKS
#define LOG_MAX_SINKS 4    /* 单 logger 最多同时订阅的 sink 数（stdout / GCS / 文件 …） */
#endif
#define LOG_MAX_TOPICS 1   /* 内部仅一个 topic：整行日志 */

/* 单个 sink 槽位（函数 + 上下文），经内部总线订阅；多 sink 可同时分发 */
typedef struct log_sink_slot {
    log_sink_fn fn;
    void *ctx;
} log_sink_slot;

/* 内部事件总线主题：把一整行格式化后的日志统一分发（payload 见 log_event） */
typedef enum {
    LOG_TOPIC_LINE = 0    /* 一整行日志（payload = log_event） */
} log_topic_t;

/* 经内部总线分发的日志事件（payload）：各 sink 最终以 log_sink_fn(level,data,len) 收到 */
typedef struct log_event {
    log_level_t  level;
    const char  *data;
    size_t       len;
} log_event;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 日志的核心能力都在虚表里，log 成为可派生/可替换行为的对象。
 * 变参无法表达为虚表方法，故由变长自由函数 log_printf 转发到 va_list 方法。
 * ------------------------------------------------------------------------- */
struct logFun {
    /* 核心日志（va_list 形式，供变长前端复用） */
    void (*vlog)(logger *self, log_level_t level, const char *tag, const char *fmt, va_list ap);
    void (*set_level)(logger *self, log_level_t level);
    log_level_t (*get_level)(const logger *self);
    /* 多 sink 分发：add_sink 追加一个订阅者（多介质并存）；set_sink 清空后只留这一个
     * （兼容旧的“切换输出”语义）。两者均不影响 log_printf 对外 API。 */
    void (*add_sink)(logger *self, log_sink_fn sink, void *ctx);
    void (*set_sink)(logger *self, log_sink_fn sink, void *ctx);
    void (*deinit)(logger *self);
    void (*destroy)(logger *self);
};

struct log {
    const struct logFun *fun;
    log_level_t level;
    /* 多 sink：内部事件总线（单 topic LOG_TOPIC_LINE，payload=log_event），
     * 各 sink 经 bus 订阅，可同时向 stdout / GCS STATUSTEXT / 文件等多介质分发 */
    bus lb;
    uint8_t lbuf[BUS_BUF_SIZE(LOG_MAX_TOPICS, LOG_MAX_SINKS)];
    log_sink_slot sinks[LOG_MAX_SINKS];   /* 与总线订阅槽位一一对应 */
    uint8_t sink_count;
    char buf[LOG_BUF_SIZE];
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
logger *log_create(void);                 /* 堆分配 + 初始化 */
void  log_init(logger *self);             /* 栈/静态分配初始化 */
void  log_deinit(logger *self);           /* 对应 init，释放对象侧资源 */
void  log_destroy(logger *self);          /* 对应 create，释放堆内存 */

/* 变长前端（自由函数）：格式化后走虚表 vlog。
 * 这是 OOC 日志的标准形态——变参无法表达为虚表方法，
 * 因此由变长自由函数转发到 va_list 形式的虚表方法。 */
void log_printf(logger *self, log_level_t level, const char *tag, const char *fmt, ...);

/* 默认 sink：写到 stdout（仅用于主机/测试，真实 MCU 请 set_sink 换成 UART 等） */
void log_default_sink(void *ctx, log_level_t level, const char *data, size_t len);

#endif /* JOC_BASE_DS_LOG_H */
