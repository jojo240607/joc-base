#ifndef JOC_BASE_DS_TIMER_H
#define JOC_BASE_DS_TIMER_H

#include <stddef.h>
#include <stdbool.h>

#include "list.h"

/* 定时器回调 */
typedef void (*timer_cb_fn)(void *arg);

/* 不透明定时器管理器 */
typedef struct timer timer;

/* 定时器条目：可静态分配或内嵌到宿主结构体（侵入式挂入管理器链表）。
 * 必须零初始化后再由 start() 接管；start() 会写全所有字段。 */
typedef struct timer_entry {
    list_node_t node;        /* 侵入式节点，便于经 LIST_ENTRY 反算父结构体 */
    uint32_t interval_ms;    /* 周期 */
    uint32_t remaining_ms;   /* 剩余时间 */
    bool periodic;           /* 周期性 / 一次性 */
    bool running;            /* 是否处于活动链表（防止回调内自 stop 造成二次摘除）*/
    timer_cb_fn cb;
    void *arg;
} timer_entry_t;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 定时器管理器复用 list 模块管理活动条目（侵入式链表），
 * 行为集中在虚表里，timer 成为可派生的对象。
 * ------------------------------------------------------------------------- */
struct timerFun {
    void (*start)(timer *self, timer_entry_t *e, uint32_t interval_ms,
                  bool periodic, timer_cb_fn cb, void *arg);
    void (*stop)(timer *self, timer_entry_t *e);
    void (*tick)(timer *self, uint32_t elapsed_ms);   /* 由周期源（systick 等）调用 */
    size_t (*count)(const timer *self);
    void (*clear)(timer *self);
    void (*deinit)(timer *self);
    void (*destroy)(timer *self);
};

struct timer {
    const struct timerFun *fun;
    list list;          /* 复用 list 模块管理活动定时器 */
    size_t active;
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
timer *timer_create(void);                /* 堆分配 + 初始化 */
void   timer_init(timer *self);          /* 栈/静态分配初始化 */
void   timer_deinit(timer *self);        /* 对应 init，释放对象侧资源 */
void   timer_destroy(timer *self);       /* 对应 create，释放堆内存 */

#endif /* JOC_BASE_DS_TIMER_H */
