#ifndef JOC_BASE_DS_TW_H
#define JOC_BASE_DS_TW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "list.h"
#include "pool.h"

/* 定时器回调 */
typedef void (*tw_cb_fn)(void *arg);

/* 不透明时间轮定时器管理器 */
typedef struct tw tw;

/* ---------------------------------------------------------------------------
 * 定时器条目：侵入式节点，可静态分配 / 内嵌宿主结构体 / 由管理器对象池分配。
 * 必须零初始化后再交给 start 使用；start 会写全所有字段。
 *
 * 时间轮采用“单级轮 + 轮转计数（rotation）”：
 *   - 每个定时器记录 remaining（当前轮内剩余 ticks，< slot_count）与
 *     rotation（还需经过的完整轮转次数）；
 *   - 插入槽位 = (current + remaining) % slot_count；
 *   - 每经过 slot_count 个 tick，current 绕回一圈，rotation 减 1；
 *   - 仅当 rotation==0 且到达槽位时才触发回调（支持任意大延迟）。
 * 这样每 tick 仅处理一个槽位链表，与活动定时器总数无关，适合管理大量定时器。
 * ------------------------------------------------------------------------- */
typedef struct tw_entry {
    list_node_t node;       /* 侵入式节点，挂入时间轮某槽位的链表 */
    uint32_t interval;      /* 周期（ticks） */
    uint32_t remaining;     /* 当前轮内剩余 ticks（< slot_count） */
    uint16_t rotation;      /* 还需经过的完整轮转次数 */
    size_t  slot;           /* 当前所在槽位下标（用于 O(1) 摘除） */
    bool periodic;          /* 周期性 / 一次性 */
    bool running;           /* 是否挂在轮上（防止回调内自 stop 造成二次摘除） */
    tw_cb_fn cb;
    void *arg;
} tw_entry_t;

/* ---------------------------------------------------------------------------
 * 函数表（OOC 虚表）
 * 时间轮管理器复用 list 模块管理每个槽位的活动条目（侵入式链表），
 * 并在 attach 对象池后复用 pool 模块分配条目节点。行为集中在虚表里，
 * tw 成为可派生的对象。
 * ------------------------------------------------------------------------- */
struct twFun {
    /* 启动一个由调用方提供的条目（静态/内嵌/池分配皆可） */
    void (*start)(tw *self, tw_entry_t *e, uint32_t interval_ticks,
                  bool periodic, tw_cb_fn cb, void *arg);
    /* 从绑定对象池分配条目并启动，返回条目指针（无池或池满返回 NULL） */
    tw_entry_t *(*start_alloc)(tw *self, uint32_t interval_ticks,
                               bool periodic, tw_cb_fn cb, void *arg);
    /* 停止（摘除）一个活动条目 */
    void (*stop)(tw *self, tw_entry_t *e);
    /* 由周期源（systick 等）调用：推进 elapsed_ticks 个 tick 并触发到期回调 */
    void (*tick)(tw *self, uint32_t elapsed_ticks);

    size_t (*count)(const tw *self);   /* 活动定时器数 */
    void (*clear)(tw *self);            /* 摘除全部（标记非活动，不释放池节点） */

    /* 条目节点分配（需先 tw_attach_pool） */
    tw_entry_t *(*alloc_entry)(tw *self);
    void (*free_entry)(tw *self, tw_entry_t *e);

    /* 生命周期（对应 init / create） */
    void (*deinit)(tw *self);
    void (*destroy)(tw *self);
};

struct tw {
    const struct twFun *fun;
    list *slots;          /* 槽位数组（调用方提供或本对象堆分配） */
    size_t slot_count;    /* 槽位数 N（每 tick 前进一格） */
    size_t current;       /* 当前槽位指针 */
    size_t active;        /* 活动定时器数 */
    pool entry_pool;      /* 可选：内嵌对象池，用于 alloc_entry / free_entry */
    bool has_pool;        /* 是否已绑定对象池 */
    bool owns_slots;      /* 槽位数组是否由本对象堆分配（destroy 时释放） */
};

/* ---- 生命周期：构造函数与工厂为自由函数（符合 OOC 惯例）---- */
tw *tw_create(size_t slot_count);                              /* 堆分配槽位 + 初始化 */
void tw_init(tw *self, list *slots, size_t slot_count);        /* 绑定调用方提供的槽位数组 */
void tw_attach_pool(tw *self, void *buf, size_t buf_size);     /* 绑定对象池后备缓冲（供 alloc_entry） */
void tw_deinit(tw *self);                                      /* 对应 init，释放对象侧资源 */
void tw_destroy(tw *self);                                     /* 对应 create，释放堆内存 */

/* 方法表实例（由 tw.c 提供并赋值给 self->fun） */
extern const struct twFun tw_fun;

#endif /* JOC_BASE_DS_TW_H */
