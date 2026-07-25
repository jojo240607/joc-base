#include "../log/log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#ifndef LOG_BUF_SIZE
#define LOG_BUF_SIZE 128
#endif

/* 级别短标记 */
static const char *log_level_char(log_level_t level) {
    switch (level) {
        case LOG_EMERGENCY: return "M";
        case LOG_ALERT:     return "A";
        case LOG_CRITICAL:  return "C";
        case LOG_ERROR:     return "E";
        case LOG_WARN:      return "W";
        case LOG_INFO:      return "I";
        case LOG_DEBUG:     return "D";
        case LOG_VERBOSE:   return "V";
        default:            return "?";
    }
}

/* 默认 sink：写到 stdout */
void log_default_sink(void *ctx, log_level_t level, const char *data, size_t len) {
    (void)ctx; (void)level;
    fwrite(data, 1, len, stdout);
}

/* ---------------------------------------------------------------------------
 * 内部事件总线订阅者：把总线上的 log_event 转交具体 sink（log_sink_fn）
 * ------------------------------------------------------------------------- */
static void log_bus_sink_dispatch(uint16_t topic, const void *data, size_t len, void *ctx) {
    (void)topic; (void)len;
    const log_event *ev = (const log_event *)data;
    log_sink_slot *s = (log_sink_slot *)ctx;
    if (s && s->fn) s->fn(s->ctx, ev->level, ev->data, ev->len);
}

/* 追加一个 sink（多消费者并存） */
static void log_add_sink_impl(logger *self, log_sink_fn sink, void *ctx) {
    if (!self || !sink || !self->lb.fun) return;
    if (self->sink_count >= LOG_MAX_SINKS) return;          /* 槽位已满，安全忽略 */
    log_sink_slot *s = &self->sinks[self->sink_count];
    s->fn = sink; s->ctx = ctx;
    bus_sub_t *h = self->lb.fun->subscribe(&self->lb, LOG_TOPIC_LINE, log_bus_sink_dispatch, s);
    if (h) self->sink_count++;
    else   s->fn = NULL;                                     /* 订阅失败，回滚槽位 */
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现
 * ------------------------------------------------------------------------- */
static void log_method_vlog(logger *self, log_level_t level, const char *tag, const char *fmt, va_list ap) {
    if (level == LOG_NONE || level > self->level) return;   /* 阈值过滤 */

    int off = snprintf(self->buf, sizeof(self->buf), "%s/%s: ",
                       log_level_char(level), tag ? tag : "?");
    if (off < 0) off = 0;
    if ((size_t)off < sizeof(self->buf)) {
        vsnprintf(self->buf + off, sizeof(self->buf) - (size_t)off, fmt, ap);
    }

    size_t len = strlen(self->buf);
    if (len > 0 && self->buf[len - 1] != '\n') {
        if (len + 1 < sizeof(self->buf)) {
            self->buf[len] = '\n';
            len++;
        }
    }
    /* 经内部事件总线向所有订阅 sink 分发同一行日志（多消费者同拍收到） */
    log_event ev = { level, self->buf, len };
    if (self->lb.fun) self->lb.fun->publish(&self->lb, LOG_TOPIC_LINE, &ev, sizeof(ev));
}

static void log_method_set_level(logger *self, log_level_t level) {
    self->level = level;
}

static log_level_t log_method_get_level(const logger *self) {
    return self->level;
}

static void log_method_add_sink(logger *self, log_sink_fn sink, void *ctx) {
    log_add_sink_impl(self, sink, ctx);
}

/* 替换式：清空现有全部 sink，仅留这一个（兼容旧“切换输出”语义） */
static void log_method_set_sink(logger *self, log_sink_fn sink, void *ctx) {
    if (!self) return;
    if (self->lb.fun) self->lb.fun->unsubscribe_topic(&self->lb, LOG_TOPIC_LINE);
    self->sink_count = 0;
    log_add_sink_impl(self, sink, ctx);
}

static void log_method_deinit(logger *self) {
    if (!self) return;
    if (self->lb.fun) bus_deinit(&self->lb);   /* 释放总线调度状态（静态缓冲不释放） */
    self->sink_count = 0;
}

static void log_method_destroy(logger *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* 方法表实例 */
const struct logFun log_fun = {
    .vlog      = log_method_vlog,
    .set_level = log_method_set_level,
    .get_level = log_method_get_level,
    .add_sink  = log_method_add_sink,
    .set_sink  = log_method_set_sink,
    .deinit    = log_method_deinit,
    .destroy   = log_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */
logger *log_create(void) {
    logger *self = (logger *)malloc(sizeof(struct log));
    if (!self) return NULL;
    log_init(self);
    return self;
}

void log_init(logger *self) {
    if (!self) return;
    self->fun = &log_fun;            /* 绑定虚表 */
    self->level = LOG_INFO;          /* 默认级别 INFO */
    self->sink_count = 0;
    memset(&self->sinks, 0, sizeof(self->sinks));
    bus_init(&self->lb, self->lbuf, sizeof(self->lbuf), LOG_MAX_TOPICS, LOG_MAX_SINKS);
    memset(self->buf, 0, sizeof(self->buf));
    log_add_sink_impl(self, log_default_sink, NULL);  /* 默认 sink：stdout */
}

void log_deinit(logger *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void log_destroy(logger *self) {
    if (!self) return;
    self->fun->destroy(self);
}

/* 变长前端：转发到 va_list 虚表方法 */
void log_printf(logger *self, log_level_t level, const char *tag, const char *fmt, ...) {
    if (!self || !self->fun) return;
    va_list ap;
    va_start(ap, fmt);
    self->fun->vlog(self, level, tag, fmt, ap);
    va_end(ap);
}
