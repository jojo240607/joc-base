#include "rtos.h"
#include "common/lock.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * jOS IPC 原语：信号量 / 互斥量(优先级天花板) / 消息队列 / 事件标志 / 对象注册表
 *
 * 阻塞 API 仅可在任务上下文调用；ISR 或内核未启动时的“阻塞”调用退化为
 * 非阻塞/忙等（见各函数头部的 fallback），以保证启动期与中断安全。
 *
 * 所有队列/计数器修改都在 irq_lock（PRIMASK）保护下进行；阻塞通过 rtos_pend
 * 让出 CPU，唤醒通过 rtos_post 路径请求 PendSV 切换，绝不忙等。
 * ------------------------------------------------------------------------- */

/* 是否在中断上下文：透过 common/lock.h 的 arch_in_isr() 探测，
 * 不直接读写任何 ISA 魔法地址（0xE000ED04），保持核心可移植。 */
static int rtos_ipc_in_isr(void) {
    return arch_in_isr();
}

/* ===========================================================================
 * 信号量
 * ========================================================================= */
void rtos_sem_init(rtos_sem_t *s, uint32_t initial, uint32_t limit) {
    if (!s) return;
    if (limit == 0) limit = 1;
    if (initial > limit) initial = limit;
    s->count = initial;
    s->limit = limit;
    s->waitq = (void *)0;
}

int rtos_sem_trywait(rtos_sem_t *s) {
    if (!s) return -1;
    unsigned st = irq_lock();
    if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
    irq_unlock(st);
    return -1;
}

int rtos_sem_wait(rtos_sem_t *s) {
    if (!s) return -1;
    /* ISR / 未启动：退化为忙等 */
    if (rtos_ipc_in_isr() || !rtos_is_started()) {
        for (;;) {
            unsigned st = irq_lock();
            if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
            irq_unlock(st);
        }
    }
    unsigned st = irq_lock();
    if (s->count > 0) { s->count--; irq_unlock(st); return 0; }
    rtos_pend(&s->waitq);     /* 无许可：阻塞；被唤醒时许可已被本任务“占有” */
    irq_unlock(st);
    return 0;
}

void rtos_sem_give(rtos_sem_t *s) {
    if (!s) return;
    unsigned st = irq_lock();
    task_t *t = (task_t *)s->waitq;
    if (t) {
        /* 有等待者：直接唤醒最高优先级者（不增 count，唤醒即“消费”） */
        t = rtos_waitq_pop_highest(&s->waitq);
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        ready_add(t);
    } else if (s->count < s->limit) {
        s->count++;
    }
    irq_unlock(st);
    if (t) rtos_schedule_request();
}

/* ===========================================================================
 * 互斥量（优先级天花板协议）
 * ========================================================================= */
void rtos_mutex_init(rtos_mutex_t *m, uint8_t ceil_prio) {
    if (!m) return;
    m->owner = (task_t *)0;
    m->ceil_prio = ceil_prio;
    m->waitq = (void *)0;
}

int rtos_mutex_trylock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started() || rtos_ipc_in_isr()) return -1;
    unsigned st = irq_lock();
    if (m->owner == (task_t *)0) {
        m->owner = rtos_running();
        rtos_set_eff_prio(rtos_running(), (rtos_running()->prio < m->ceil_prio)
                                      ? rtos_running()->prio : m->ceil_prio);
        irq_unlock(st);
        return 0;
    }
    irq_unlock(st);
    return -1;
}

int rtos_mutex_lock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started() || rtos_ipc_in_isr()) return -1;
    unsigned st = irq_lock();
    if (m->owner == rtos_running()) { irq_unlock(st); return -1; }   /* 不支持递归 */
    if (m->owner == (task_t *)0) {
        m->owner = rtos_running();
        rtos_set_eff_prio(rtos_running(), (rtos_running()->prio < m->ceil_prio)
                                      ? rtos_running()->prio : m->ceil_prio);
        irq_unlock(st);
        return 0;
    }
    /* 有竞争：阻塞（天花板协议下持有者已在 ceil_prio 运行，反转被限界） */
    rtos_pend(&m->waitq);
    irq_unlock(st);
    return 0;   /* 被唤醒即成为新 owner（unlock 的 handoff 已置 owner=本任务） */
}

int rtos_mutex_unlock(rtos_mutex_t *m) {
    if (!m || !rtos_is_started() || rtos_ipc_in_isr()) return -1;
    unsigned st = irq_lock();
    if (m->owner != rtos_running()) { irq_unlock(st); return -1; }
    /* 恢复自身优先级到原始值 */
    rtos_set_eff_prio(rtos_running(), rtos_running()->base_prio);
    /* handoff：唤醒最高优先级等待者并立为 owner（提升到天花板） */
    task_t *t = rtos_waitq_pop_highest(&m->waitq);
    if (t) {
        m->owner = t;
        t->wait_obj = (void *)0;
        t->state = TASK_READY;
        rtos_set_eff_prio(t, (t->prio < m->ceil_prio) ? t->prio : m->ceil_prio);
        ready_add(t);
        irq_unlock(st);
        rtos_schedule_request();
    } else {
        m->owner = (task_t *)0;
        irq_unlock(st);
    }
    return 0;
}

/* ===========================================================================
 * 消息队列（定长项环形缓冲）
 * ========================================================================= */
void rtos_mq_init(rtos_mq_t *q, void *buf, size_t item_size, size_t cap) {
    if (!q) return;
    q->buf = (uint8_t *)buf;
    q->item_size = item_size;
    q->cap = cap;
    q->count = 0;
    q->head = 0;
    q->recv_waitq = (void *)0;
    q->send_waitq = (void *)0;
}

static void mq_push(rtos_mq_t *q, const void *item) {
    uint8_t *dst = q->buf + q->head * q->item_size;
    memcpy(dst, item, q->item_size);
    q->head = (q->head + 1) % q->cap;
    q->count++;
}
static void mq_pop(rtos_mq_t *q, void *item) {
    size_t tail = (q->head + q->cap - q->count) % q->cap;
    uint8_t *src = q->buf + tail * q->item_size;
    memcpy(item, src, q->item_size);
    q->count--;
}

int rtos_mq_trysend(rtos_mq_t *q, const void *item) {
    if (!q) return -1;
    unsigned st = irq_lock();
    if (q->count < q->cap) {
        mq_push(q, item);
        if (q->recv_waitq) {                  /* 有接收者空等：唤醒一个去取 */
            task_t *t = rtos_waitq_pop_highest(&q->recv_waitq);
            t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
            irq_unlock(st);
            rtos_schedule_request();
            return 0;
        }
        irq_unlock(st);
        return 0;
    }
    irq_unlock(st);
    return -1;
}

int rtos_mq_tryrecv(rtos_mq_t *q, void *item) {
    if (!q) return -1;
    unsigned st = irq_lock();
    if (q->count > 0) {
        mq_pop(q, item);
        if (q->send_waitq) {                  /* 有发送者满等：唤醒一个去填 */
            task_t *t = rtos_waitq_pop_highest(&q->send_waitq);
            t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
            irq_unlock(st);
            rtos_schedule_request();
            return 0;
        }
        irq_unlock(st);
        return 0;
    }
    irq_unlock(st);
    return -1;
}

int rtos_mq_send(rtos_mq_t *q, const void *item) {
    if (!q) return -1;
    if (rtos_ipc_in_isr() || !rtos_is_started()) return rtos_mq_trysend(q, item);
    for (;;) {
        unsigned st = irq_lock();
        if (q->count < q->cap) {
            mq_push(q, item);
            if (q->recv_waitq) {
                task_t *t = rtos_waitq_pop_highest(&q->recv_waitq);
                t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
                irq_unlock(st);
                rtos_schedule_request();
                return 0;
            }
            irq_unlock(st);
            return 0;
        }
        rtos_pend(&q->send_waitq);   /* 满：阻塞，被接收者唤醒后重试 */
        irq_unlock(st);
    }
}

int rtos_mq_recv(rtos_mq_t *q, void *item) {
    if (!q) return -1;
    if (rtos_ipc_in_isr() || !rtos_is_started()) return rtos_mq_tryrecv(q, item);
    for (;;) {
        unsigned st = irq_lock();
        if (q->count > 0) {
            mq_pop(q, item);
            if (q->send_waitq) {
                task_t *t = rtos_waitq_pop_highest(&q->send_waitq);
                t->wait_obj = (void *)0; t->state = TASK_READY; ready_add(t);
                irq_unlock(st);
                rtos_schedule_request();
                return 0;
            }
            irq_unlock(st);
            return 0;
        }
        rtos_pend(&q->recv_waitq);   /* 空：阻塞，被发送者唤醒后重试 */
        irq_unlock(st);
    }
}

/* ===========================================================================
 * 事件标志
 * ========================================================================= */
void rtos_event_init(rtos_event_t *e) {
    if (!e) return;
    e->flags = 0;
    e->waitq = (void *)0;
}
void rtos_event_clear(rtos_event_t *e, uint32_t bits) {
    if (!e) return;
    unsigned st = irq_lock();
    e->flags &= ~bits;
    irq_unlock(st);
}
void rtos_event_set(rtos_event_t *e, uint32_t bits) {
    if (!e) return;
    unsigned st = irq_lock();
    e->flags |= bits;
    int awoke = 0;
    task_t *t = (task_t *)e->waitq;
    while (t) {
        task_t *nx = t->wait_next;
        int sat = t->wait_mode
                  ? ((e->flags & t->wait_mask) == t->wait_mask)   /* ALL */
                  : ((e->flags & t->wait_mask) != 0);            /* ANY */
        if (sat) {
            rtos_waitq_remove(&e->waitq, t);
            t->wait_obj = (void *)0; t->wait_mask = 0; t->wait_mode = 0;
            t->state = TASK_READY; ready_add(t); awoke = 1;
        }
        t = nx;
    }
    irq_unlock(st);
    if (awoke) rtos_schedule_request();
}

uint32_t rtos_event_wait(rtos_event_t *e, uint32_t mask, int wait_all, int block) {
    if (!e || rtos_ipc_in_isr() || !rtos_is_started()) return e ? e->flags : 0;
    unsigned st = irq_lock();
    int sat = wait_all ? ((e->flags & mask) == mask) : ((e->flags & mask) != 0);
    if (sat) { irq_unlock(st); return e->flags; }
    if (!block) { irq_unlock(st); return (uint32_t)-1; }
    rtos_running()->wait_mask = mask;
    rtos_running()->wait_mode = wait_all ? 1 : 0;
    rtos_pend(&e->waitq);
    irq_unlock(st);
    rtos_running()->wait_mask = 0; rtos_running()->wait_mode = 0;
    return e->flags;   /* 被唤醒即条件满足 */
}

/* ===========================================================================
 * 内核对象注册表（调试/按名查找）
 * ========================================================================= */
#define KOBJ_MAX 24
static struct { const char *name; rtos_kobj_type_t type; void *ptr; } g_kobj[KOBJ_MAX];
static int g_kobj_n = 0;

int rtos_kobj_register(const char *name, rtos_kobj_type_t type, void *ptr) {
    if (!name || !ptr || g_kobj_n >= KOBJ_MAX) return 0;
    for (int i = 0; i < g_kobj_n; i++)
        if (strcmp(g_kobj[i].name, name) == 0) { g_kobj[i].ptr = ptr; return 1; }
    g_kobj[g_kobj_n].name = name;
    g_kobj[g_kobj_n].type = type;
    g_kobj[g_kobj_n].ptr  = ptr;
    g_kobj_n++;
    return 1;
}
void *rtos_kobj_lookup(const char *name) {
    if (!name) return (void *)0;
    for (int i = 0; i < g_kobj_n; i++)
        if (strcmp(g_kobj[i].name, name) == 0) return g_kobj[i].ptr;
    return (void *)0;
}
void rtos_kobj_foreach(void (*cb)(const char *name, rtos_kobj_type_t type, void *ptr)) {
    if (!cb) return;
    for (int i = 0; i < g_kobj_n; i++)
        cb(g_kobj[i].name, g_kobj[i].type, g_kobj[i].ptr);
}
