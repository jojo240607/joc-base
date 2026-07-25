#include "tw.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * 私有辅助
 * ------------------------------------------------------------------------- */

/* 根据延迟计算 rotation / remaining，并写回条目 */
static void tw_arm(tw *self, tw_entry_t *e, uint32_t delay) {
    if (delay == 0) delay = 1;   /* 防御：0 视为 1 tick，避免 remaining==0 落入死区 */
    uint32_t rot = delay / (uint32_t)self->slot_count;
    uint32_t rem = delay % (uint32_t)self->slot_count;
    if (rem == 0 && rot > 0) { rem = (uint32_t)self->slot_count; rot--; }  /* 整轮：留待下轮触发 */
    e->rotation  = (uint16_t)rot;
    e->remaining = rem;
}

/* 计算槽位并尾插到该槽位链表（O(1)） */
static void tw_insert(tw *self, tw_entry_t *e) {
    size_t slot = (self->current + e->remaining) % self->slot_count;
    e->slot = slot;
    list_push_back(&self->slots[slot], &e->node);
}

/* 处理当前槽位：遍历该槽位链表，处理到期 / 轮转节点。
 * 采用“缓存 next 后变更”的遍历方式，对回调中新增/摘除节点均安全：
 * 回调中新加入当前槽位的节点被追加到哨兵之前，不会被本 tick 误处理。 */
static void tw_process_slot(tw *self, size_t idx) {
    list *slot = &self->slots[idx];
    list_node_t *pos = slot->fun->first(slot);
    while (pos) {
        list_node_t *n = slot->fun->next(slot, pos);   /* 先缓存 next，再允许回调改链 */
        tw_entry_t *e = LIST_ENTRY(pos, tw_entry_t, node);

        if (e->rotation > 0) {
            /* 还需轮转：减 1 后留在同槽位，待下次绕回再处理 */
            e->rotation--;
            slot->fun->remove(slot, &e->node);
            list_push_back(slot, &e->node);            /* 同槽位尾插（下轮处理）*/
            e->slot = idx;
        } else {
            /* 到期：先从轮上摘下（running 仍为真，供回调内自 stop 判定） */
            slot->fun->remove(slot, &e->node);
            if (e->cb) e->cb(e->arg);                  /* 触发回调（可能 stop / start 其它条目）*/
            if (e->periodic) {
                /* 周期性：若回调未停止它，则重装并重新入轮（running 保持真） */
                if (e->running) {
                    tw_arm(self, e, e->interval);
                    tw_insert(self, e);
                }
                /* 否则回调已 stop 它（running 已假、计数已减），不再处理 */
            } else {
                /* 一次性：若回调未停止它，则标记离轮并减计数 */
                if (e->running) {
                    e->running = false;
                    if (self->active > 0) self->active--;
                }
                /* 否则回调已 stop 它，计数已由 stop 处理 */
            }
        }
        pos = n;
    }
}

/* ---------------------------------------------------------------------------
 * vtable 方法实现
 * ------------------------------------------------------------------------- */
static void tw_method_start(tw *self, tw_entry_t *e, uint32_t interval_ticks,
                            bool periodic, tw_cb_fn cb, void *arg) {
    if (!e || interval_ticks == 0 || self->slot_count == 0) return;
    if (e->running) return;          /* 仍在活动：需先 stop 再 start（防止重复入轮） */
    e->interval  = interval_ticks;
    e->periodic  = periodic;
    e->cb        = cb;
    e->arg       = arg;
    e->running   = true;
    tw_arm(self, e, interval_ticks);
    tw_insert(self, e);
    self->active++;
}

static tw_entry_t *tw_method_start_alloc(tw *self, uint32_t interval_ticks,
                                         bool periodic, tw_cb_fn cb, void *arg) {
    tw_entry_t *e = self->fun->alloc_entry(self);
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    self->fun->start(self, e, interval_ticks, periodic, cb, arg);
    return e;
}

static void tw_method_stop(tw *self, tw_entry_t *e) {
    if (!e || !e->running) return;   /* 已摘除则跳过，避免回调内自 stop 的二次摘除 */
    self->slots[e->slot].fun->remove(&self->slots[e->slot], &e->node);
    e->running = false;
    if (self->active > 0) self->active--;
}

static void tw_method_tick(tw *self, uint32_t elapsed_ticks) {
    if (self->slot_count == 0) return;
    for (uint32_t i = 0; i < elapsed_ticks; i++) {
        self->current = (self->current + 1U) % self->slot_count;
        tw_process_slot(self, self->current);
    }
}

static size_t tw_method_count(const tw *self) {
    return self->active;
}

static void tw_method_clear(tw *self) {
    for (size_t i = 0; i < self->slot_count; i++) {
        list *slot = &self->slots[i];
        list_node_t *pos = slot->fun->first(slot);
        while (pos) {
            list_node_t *n = slot->fun->next(slot, pos);
            tw_entry_t *e = LIST_ENTRY(pos, tw_entry_t, node);
            e->running = false;       /* 标记离轮，防止残留 running 误判 */
            pos = n;
        }
        slot->fun->clear(slot);
    }
    self->active = 0;
}

static tw_entry_t *tw_method_alloc_entry(tw *self) {
    if (!self->has_pool) return NULL;
    return (tw_entry_t *)self->entry_pool.fun->alloc(&self->entry_pool);
}

static void tw_method_free_entry(tw *self, tw_entry_t *e) {
    if (self->has_pool && e) self->entry_pool.fun->free(&self->entry_pool, e);
}

static void tw_method_deinit(tw *self) {
    if (!self) return;
    self->fun->clear(self);
    if (self->owns_slots && self->slots) {
        free(self->slots);
        self->slots = NULL;
    }
    if (self->has_pool) {
        pool_deinit(&self->entry_pool);
        self->has_pool = false;
    }
    self->slot_count = 0;
}

static void tw_method_destroy(tw *self) {
    if (!self) return;
    self->fun->deinit(self);
    free(self);
}

/* ---------------------------------------------------------------------------
 * 方法表实例
 * ------------------------------------------------------------------------- */
const struct twFun tw_fun = {
    .start        = tw_method_start,
    .start_alloc  = tw_method_start_alloc,
    .stop         = tw_method_stop,
    .tick         = tw_method_tick,
    .count        = tw_method_count,
    .clear        = tw_method_clear,
    .alloc_entry  = tw_method_alloc_entry,
    .free_entry   = tw_method_free_entry,
    .deinit       = tw_method_deinit,
    .destroy      = tw_method_destroy,
};

/* ---------------------------------------------------------------------------
 * 构造函数 / 工厂（自由函数）
 * ------------------------------------------------------------------------- */
tw *tw_create(size_t slot_count) {
    if (slot_count == 0) return NULL;
    tw *self = (tw *)malloc(sizeof(tw));
    if (!self) return NULL;

    list *slots = (list *)malloc(slot_count * sizeof(list));
    if (!slots) { free(self); return NULL; }

    self->fun        = &tw_fun;
    self->slots      = slots;
    self->slot_count = slot_count;
    self->current    = 0;
    self->active     = 0;
    self->has_pool   = false;
    self->owns_slots = true;
    for (size_t i = 0; i < slot_count; i++) list_init(&slots[i]);
    return self;
}

void tw_init(tw *self, list *slots, size_t slot_count) {
    if (!self || !slots || slot_count == 0) return;
    self->fun        = &tw_fun;
    self->slots      = slots;
    self->slot_count = slot_count;
    self->current    = 0;
    self->active     = 0;
    self->has_pool   = false;
    self->owns_slots = false;
    for (size_t i = 0; i < slot_count; i++) list_init(&slots[i]);
}

void tw_attach_pool(tw *self, void *buf, size_t buf_size) {
    if (!self || !buf || buf_size == 0) return;
    pool_init(&self->entry_pool, buf, buf_size, sizeof(tw_entry_t));
    /* pool_init 在缓冲不足时 capacity 为 0，视为未成功绑定 */
    self->has_pool = (self->entry_pool.capacity > 0);
}

void tw_deinit(tw *self) {
    if (!self) return;
    self->fun->deinit(self);
}

void tw_destroy(tw *self) {
    if (!self) return;
    self->fun->destroy(self);
}
