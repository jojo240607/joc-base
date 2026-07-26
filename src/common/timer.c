#include "timer.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * vtable 方法实现
 * ------------------------------------------------------------------------- */
static void timer_method_start(timer *self, timer_entry_t *e, uint32_t interval_ms,
                               bool periodic, timer_cb_fn cb, void *arg) {
    if (!e || interval_ms == 0) return;
    e->interval_ms  = interval_ms;
    e->remaining_ms = interval_ms;
    e->periodic     = periodic;
    e->cb           = cb;
    e->arg          = arg;
    e->running      = true;

    /* 尾插到活动链表 */
    self->list.fun->insert_after(&self->list, &self->list.head, &e->node);
    self->active++;
}

static void timer_method_stop(timer *self, timer_entry_t *e) {
    if (!e || !e->running) return;       /* 已摘除则跳过，避免回调内自 stop 的二次摘除 */
    self->list.fun->remove(&self->list, &e->node);
    e->running = false;
    if (self->active > 0) self->active--;
}

static void timer_method_tick(timer *self, uint32_t elapsed_ms) {
    list_node_t *pos = self->list.fun->first(&self->list);
    while (pos) {
        /* 先缓存 next，再触发回调——回调中可能 stop/start 其它条目，不影响安全遍历 */
        list_node_t *n = self->list.fun->next(&self->list, pos);
        timer_entry_t *e = LIST_ENTRY(pos, timer_entry_t, node);

        if (elapsed_ms >= e->remaining_ms) {
            if (e->cb) e->cb(e->arg);
            if (e->periodic) {
                e->remaining_ms = e->interval_ms;     /* 周期重装 */
            } else if (e->running) {
                self->fun->stop(self, e);             /* 一次性：摘除（running 防重入）*/
            }
        } else {
            e->remaining_ms -= elapsed_ms;
        }
        pos = n;
    }
}

static size_t timer_method_count(const timer *self) {
    return self->active;
}

static void timer_method_clear(timer *self) {
    self->list.fun->clear(&self->list);
    self->active = 0;
}

static void timer_method_deinit(timer *self) {
    (void)self;   /* 对象侧无可释放资源（条目由调用方管理）*/
}

static void timer_method_destroy(timer *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* 方法表实例 */
const struct timerFun timer_fun = {
    .start  = timer_method_start,
    .stop   = timer_method_stop,
    .tick   = timer_method_tick,
    .count  = timer_method_count,
    .clear  = timer_method_clear,
    .deinit = timer_method_deinit,
    .destroy= timer_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数，符合 OOC 惯例）
 * ------------------------------------------------------------------------- */
timer *timer_create(void) {
    timer *self = (timer *)malloc(sizeof(timer));
    if (!self) return NULL;
    timer_init(self);
    return self;
}

void timer_init(timer *self) {
    if (!self) return;
    self->fun = &timer_fun;             /* 绑定虚表 */
    list_init(&self->list);             /* 初始化内部活动链表 */
    self->active = 0;
}

void timer_deinit(timer *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void timer_destroy(timer *self) {
    if (!self) return;
    self->fun->destroy(self);
}
